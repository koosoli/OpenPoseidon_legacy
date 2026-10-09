import random
import struct
from array import array
import unittest
import math
from build_fusion import compressed, translate_objects, fixed, ISLANDS, world_data, array_entry, \
    env_sounds, island_env_sounds, sound_regions, sea_floor, CELL, SIZE


class FusionTests(unittest.TestCase):
    def test_tiles_separate_and_fit(self):
        for i, (_, _, x, z) in enumerate(ISLANDS):
            self.assertGreaterEqual(x, 0); self.assertGreaterEqual(z, 0)
            self.assertLessEqual(x+256, SIZE); self.assertLessEqual(z+256, SIZE)
            for _, _, xx, zz in ISLANDS[i+1:]:
                self.assertTrue(x+256+64 <= xx or xx+256+64 <= x or
                                z+256+64 <= zz or zz+256+64 <= z)

    def test_layout_follows_the_campaign(self):
        centre = {key: (x + 128, z + 128) for key, _, x, z in ISLANDS}
        def bearing(a, b):  # degrees clockwise from north, a to b
            dx, dz = centre[b][0] - centre[a][0], centre[b][1] - centre[a][1]
            return math.degrees(math.atan2(dx, dz)) % 360
        self.assertTrue(22.5 <= bearing('abel', 'eden') <= 67.5)       # Everon north-east of Malden (01flashpoint)
        self.assertEqual(max(centre[k][0] for k in ('abel', 'eden', 'cain', 'noe')), centre['eden'][0])  # easternmost
        self.assertTrue(45 <= bearing('abel', 'cain') <= 90)           # Kolgujev east / north-east of Malden (31a)
        self.assertTrue(315 <= bearing('abel', 'noe') <= 350)          # Nogova north-north-west of Malden (Reforger)
        self.assertTrue(135 <= bearing('noe', 'abel') <= 225)          # US reach Nogova from the south (x12gameover)
        self.assertTrue(90 <= bearing('noe', 'cain') <= 180)           # Soviet Su-25s from the south-east (x13badend)
        # 31bmaverick's A-10 start (Kolgujev 15291,4271) lies east of Malden's runway (31a, 7921,9635): "heading east"
        tile = {key: (x, z) for key, _, x, z in ISLANDS}
        start = (tile['cain'][0] * CELL + 15291, tile['cain'][1] * CELL + 4271)
        runway = (tile['abel'][0] * CELL + 7921, tile['abel'][1] * CELL + 9635)
        self.assertTrue(70 <= math.degrees(math.atan2(start[0] - runway[0], start[1] - runway[1])) % 360 <= 110)
        # Everon a neighbour, not out on its own: its tile no further from Kolgujev's than Malden's is from
        # Nogova's (the group's other neighbours)
        def gap(a, b):
            (ax, az), (bx, bz) = tile[a], tile[b]
            return max(bx - (ax + 256), ax - (bx + 256), bz - (az + 256), az - (bz + 256))
        self.assertLessEqual(gap('eden', 'cain'), gap('abel', 'noe') * 1.25)

    def test_airport_and_names_from_config_text(self):
        cfg = """class CfgWorlds { class DefaultWorld {}; class Eden: DefaultWorld {
            ilsPosition[]={4772,10923}; ilsDirection[]={0,0.080000,-1};
            ilsTaxiIn[]={"4772+69","10923+400",4772,10923}; ilsTaxiOff[]={};
            class Sounds {}; class Names { class Regina { name="Regina"; position[]={7197,2347,50}; }; }; }; };"""
        airport, names = world_data([cfg], 'eden', 1000, 2000)
        self.assertEqual(airport['pos'], [5772, 12923])
        self.assertEqual(airport['taxi_in'], [5841, 13323, 5772, 12923])
        self.assertEqual(names[0]['name'], 'Regina')
        self.assertEqual(names[0]['position'], [8197, 4347, 50])
        self.assertEqual(world_data([cfg], 'cain', 0, 0), (None, []))

    def test_each_island_keeps_its_ambient_sounds(self):
        cfg = """class CfgWorlds { class DefaultWorld { class EnvSounds: CfgEnvSounds {}; };
            class Eden: DefaultWorld { class Sounds {}; };
            class Abel: DefaultWorld { class EnvSounds: CfgEnvSounds {
                class Sea { sound[]={"Environmental\\wave_break",0.0001,1}; }; }; class Names {}; }; };"""
        self.assertIn('wave_break', env_sounds([cfg], 'abel'))
        self.assertEqual(env_sounds([cfg], 'eden'), '')
        regions = sound_regions([cfg])
        self.assertEqual(regions.count('class EnvSounds: CfgEnvSounds'), len(ISLANDS))
        ox, oz = ISLANDS[1][2] * CELL, ISLANDS[1][3] * CELL
        self.assertIn('class Malden {area[]={%d,%d,%d,%d}; class EnvSounds: CfgEnvSounds {class Sea'
                      % (ox, oz, ox + 256 * CELL, oz + 256 * CELL), regions)
        # Everon keeps CfgEnvSounds; the islands without their own get Fusion's, all different
        self.assertIn('class Everon {area[]={%d,%d,%d,%d}; class EnvSounds: CfgEnvSounds {};};'
                      % (ISLANDS[0][2] * CELL, ISLANDS[0][3] * CELL, (ISLANDS[0][2] + 256) * CELL,
                         (ISLANDS[0][3] + 256) * CELL), regions)
        sets = [island_env_sounds([cfg], key) for key, _, _, _ in ISLANDS]
        self.assertEqual(len(set(sets)), len(ISLANDS))
        self.assertIn('class Hills { sound[]={"environmental\\wind_thick",0.000126,1};', sets[2])

    def test_sea_floor_has_no_cliffs_or_stray_land(self):
        size, ox, oz = 800, 272, 272
        rng = random.Random(7)
        heights = array('f', [-100]) * (size * size)
        for z in range(256):  # an island in the middle, a varied sea bed 20-90 m deep round it
            for x in range(256):
                depth = -55 + 30 * math.sin(x / 13) * math.cos(z / 17) + rng.uniform(-2, 2)
                heights[(oz + z) * size + ox + x] = 80.0 if min(x, z, 255 - x, 255 - z) > 60 else depth
        heights[(oz + 255) * size + ox + 100] = 3.6  # a source edge above water, like Nogova's north
        island = array('f', heights)
        sea_floor(heights, size, [(ox, oz)])
        inside = lambda x, z: ox <= x < ox + 256 and oz <= z < oz + 256
        worst = 0
        for z in range(size):
            for x in range(size):
                h = heights[z * size + x]
                if inside(x, z):
                    self.assertEqual(h, island[z * size + x])  # the island's own data is untouched
                    continue
                self.assertLess(h, -1.0)                       # no land outside the source maps
                self.assertGreaterEqual(h, -400.0)
                if abs(x - (ox + 100)) <= 4 and abs(z - (oz + 255)) <= 4:
                    continue  # the beach below the spike: checked by assertLess above
                for nx, nz in ((x + 1, z), (x, z + 1)):
                    if nx < size and nz < size:
                        worst = max(worst, abs(h - heights[nz * size + nx]))
        self.assertLess(worst, 12.0)  # metres per 50 m cell: was an 80 m wall at the tile edge
        far = [heights[z * size + 80] for z in range(80, size - 80)]   # 9.6 km from the tile: the basin
        self.assertTrue(-400 <= min(far) and max(far) <= -200)
        rim = [heights[z * size] for z in range(size)] + [heights[x] for x in range(size)]
        self.assertEqual(set(rim), {-100.0})  # the world's edge meets the engine's ground beyond the map

    def test_placement_preserves_rotation_height_name_and_unique_id(self):
        matrix = (0.,0.,-1., 0.,1.,0., 1.,0.,0., 100.,23.,200.)
        record=struct.pack('<12fI',*matrix,123)+fixed('data3d\\test.p3d',76)
        result=translate_objects(record*2,3200,8000,450000)
        for i in range(2):
            values=struct.unpack_from('<12fI',result,i*128)
            self.assertEqual(values[:9],matrix[:9])
            self.assertEqual(values[9:],(3300.,23.,8200.,450000+i))
            self.assertEqual(result[i*128+52:(i+1)*128],record[52:])

    def test_literal_stream_checksum_and_partial_groups(self):
        for size in (0,1023,1024,1025,4096):
            raw=bytes(i%251 for i in range(size))
            encoded=compressed(raw)
            if size < 1024:
                self.assertEqual(raw,encoded); continue
            out=bytearray(); at=0
            while len(out)<size:
                self.assertEqual(encoded[at],255); at+=1
                n=min(8,size-len(out)); out.extend(encoded[at:at+n]); at+=n
            self.assertEqual(bytes(out),raw)
            self.assertEqual(struct.unpack_from('<I',encoded,at)[0],sum(raw))
            self.assertEqual(len(encoded),at+4)


if __name__ == '__main__': unittest.main()
