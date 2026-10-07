"""Privacy edits must not change binary save layout or adjacent data."""
import unittest

from prepare_clean_candidate import anonymize_save
from export_source import scan


class SaveSanitationTests(unittest.TestCase):
    def test_only_source_path_changes_with_identical_layout(self):
        prefix = b'\x00\xfffixture\x00'
        path = b'F:/ofpisnotdead.com/sources/CWA/OFPR/tests/integration/missions/'
        suffix = b'\x00\x01\x00mission\x00command_menu_basic\x00'
        original = prefix + path + suffix
        clean = anonymize_save(original)
        self.assertEqual(len(clean), len(original))
        self.assertEqual(clean[:len(prefix)], prefix)
        self.assertEqual(clean[-len(suffix):], suffix)
        self.assertEqual([i for i, b in enumerate(original) if b == 0],
                         [i for i, b in enumerate(clean) if b == 0])
        self.assertFalse(scan(clean))

    def test_unexpected_input_is_rejected(self):
        with self.assertRaises(ValueError):
            anonymize_save(b'no source path\0')


if __name__ == '__main__':
    unittest.main()
