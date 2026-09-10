import io
import struct
import unittest
import build_showcase as lab


class ShowcaseAssets(unittest.TestCase):
    def test_deterministic_package_and_sizes(self):
        payload = lab.build()
        self.assertEqual(payload, lab.build())
        stream = io.BytesIO(payload)

        def string():
            result = bytearray()
            while True:
                char = stream.read(1)
                self.assertTrue(char, 'unterminated PBO string')
                if char == b'\0':
                    return result.decode('ascii')
                result += char

        self.assertEqual(string(), '')
        self.assertEqual(struct.unpack('<5I', stream.read(20))[0], 0x56657273)
        self.assertEqual((string(), string(), string()), ('prefix', 'showcase_lab', ''))
        files = []
        while True:
            name = string()
            method, original, reserved, timestamp, size = struct.unpack('<5I', stream.read(20))
            if not name:
                break
            self.assertEqual((method, reserved, timestamp), (0, 0, 0))
            self.assertEqual(original, size)
            files.append((name, size))
        content = {name: stream.read(size) for name, size in files}
        self.assertEqual(stream.read(), b'')
        self.assertEqual(len(content), 29)
        self.assertIn(b'class LabPanelGlass', content['config.cpp'])
        config = content['config.cpp'].decode('ascii')
        glass = config.split('class LabPanelGlass:', 1)[1].split('};', 1)[0]
        self.assertIn('armor=1;', glass)
        self.assertIn('removeOnDestruction=1;', glass)
        self.assertEqual(config.count('removeOnDestruction=1;'), 1)
        self.assertEqual(config.count('armor=10000;'), 10)
        self.assertIn(b'PanelGlass_ca.paa', content['PanelGlass.p3d'])
        self.assertIn(b'class LabDoorSide', content['config.cpp'])
        self.assertIn(b'class LabPanelNormal', content['config.cpp'])
        for name, data in content.items():
            if name.endswith('.p3d'):
                self.assertEqual(data[:12], b'MLOD' + struct.pack('<II', 257, 4))
                self.assertEqual(data.count(b'Component01'), 3)

    def test_normal_map_has_two_nonflat_channels(self):
        pixels = lab.tga(True)[18:]
        self.assertEqual(len(pixels), 128 * 128 * 4)
        self.assertGreater(len(set(pixels[1::4])), 8)
        self.assertGreater(len(set(pixels[3::4])), 8)
        self.assertIn(b'ribs_nohq.paa', lab.material(True, .05))
        self.assertNotIn(b'ribs_nohq.paa', lab.material(False, .05))
        self.assertEqual(lab.paa()[:4], struct.pack('<HH', 0x8888, 0))

    def test_glass_has_continuous_partial_alpha_through_all_mips(self):
        data = lab.paa(colour=(220, 235, 200, 56))
        offset, levels = 4, 0
        while data[offset:offset+4] != bytes(4):
            width, height = struct.unpack_from('<HH', data, offset)
            size = int.from_bytes(data[offset+4:offset+7], 'little')
            pixels = data[offset+7:offset+7+size]
            self.assertEqual(size, width * height * 4)
            self.assertEqual(set(pixels[3::4]), {56})
            self.assertEqual(pixels[:4], bytes((220, 235, 200, 56)))
            offset += 7 + size
            levels += 1
        self.assertEqual(levels, 8)

    def test_visible_and_collision_shell_winding_match(self):
        def first_face(geometry):
            data = lab.box_lod((7, .3, 7), geometry=geometry)
            at = 28 + 8 * 16 + 6 * 12 + 4
            return [struct.unpack_from('<I', data, at + i * 16)[0] for i in range(4)]
        visible = first_face(False)
        collision = first_face(True)
        self.assertEqual(visible, collision)
        self.assertEqual(collision, [1, 0, 3, 2])
        # Loader reverses each quad to the outward bottom face.
        loaded = [visible[1], visible[0], visible[3], visible[2]]
        points = [(-7, 0, -7), (7, 0, -7), (7, 0, 7), (-7, 0, 7)]
        a, b, c = (points[i] for i in loaded[:3])
        cross_y = (b[2]-a[2])*(c[0]-a[0]) - (b[0]-a[0])*(c[2]-a[2])
        self.assertLess(cross_y, 0)


if __name__ == '__main__':
    unittest.main()
