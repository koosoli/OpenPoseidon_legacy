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
        self.assertEqual(len(content), 18)
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


if __name__ == '__main__':
    unittest.main()
