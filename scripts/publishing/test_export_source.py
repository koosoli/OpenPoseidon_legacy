"""Guard tests for private release preparation; no network or game assets."""
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
import export_source as subject


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.repo = self.base / 'repo'
        self.repo.mkdir()
        self.git('init', '-q')
        self.git('config', 'user.name', 'Test')
        self.git('config', 'user.email', 'test@example.invalid')
        (self.repo / 'source.cpp').write_bytes(b'int pinned = 1;\n')
        (self.repo / 'private.txt').write_bytes(b'internal\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.commit = self.git('rev-parse', 'HEAD').decode().strip()
        self.policy = self.base / 'policy.json'
        self.policy.write_text(json.dumps(dict(rewrites={}, add_files={}, include_files=['source.cpp'],
                                              include_prefixes=[], exclude_prefixes=[],
                                              exclude_files=['private.txt'], license_holds=[])), encoding='utf-8')
        self.reports = self.base / 'reports'
        subject.inventory(self.repo, self.commit, self.policy, self.reports)
        self.manifest = self.reports / 'inventory.json'
        self.output = self.base / 'export'

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.repo), *args], stderr=subprocess.STDOUT)

    def draft(self):
        return subject.export(self.repo, self.manifest, self.policy, self.output, True)

    def test_reads_pinned_git_blob_not_worktree(self):
        (self.repo / 'source.cpp').write_bytes(b'int uncommitted = 2;\n')
        self.draft()
        self.assertEqual((self.output / 'source.cpp').read_bytes(), b'int pinned = 1;\n')
        self.assertFalse((self.output / 'private.txt').exists())
        self.assertFalse((self.output / '.git').exists())

    def test_final_export_rejects_pending_reviews(self):
        with self.assertRaisesRegex(ValueError, 'final export blocked'):
            subject.export(self.repo, self.manifest, self.policy, self.output, False)
        self.assertFalse(self.output.exists())

    def test_policy_change_rejected(self):
        self.policy.write_bytes(self.policy.read_bytes() + b' ')
        with self.assertRaisesRegex(ValueError, 'Policy changed'):
            self.draft()

    def test_existing_destination_never_overwritten(self):
        self.output.mkdir()
        (self.output / 'keep.txt').write_bytes(b'keep')
        with self.assertRaisesRegex(ValueError, 'existing files'):
            self.draft()
        self.assertEqual((self.output / 'keep.txt').read_bytes(), b'keep')

    def test_export_inside_source_rejected(self):
        self.output = self.repo / 'nested'
        with self.assertRaisesRegex(ValueError, 'outside the private worktree'):
            self.draft()

    def test_unexpected_git_metadata_rejected(self):
        self.draft()
        (self.output / '.git').mkdir()
        (self.output / '.git' / 'config').write_bytes(b'foreign remote')
        with self.assertRaisesRegex(ValueError, 'file set/hashes'):
            subject.verify(self.manifest, self.output)

    def test_tampered_export_rejected(self):
        self.draft()
        (self.output / 'source.cpp').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'file set/hashes'):
            subject.verify(self.manifest, self.output)

    def test_unsafe_paths_rejected(self):
        for name in ('../leak', '/absolute', '.Git/config', 'a/../b', 'a//b',
                     'a/./b', 'C:/Users/user', 'a\\b', 'NUL.txt', 'a/b. '):
            with self.subTest(name=name), self.assertRaises(ValueError):
                subject.safe_path(name)

    def test_binary_private_path_detected_without_value(self):
        result = subject.scan(b'\x00F:/ofpisnotdead.com/sources/CWA\x00')
        self.assertTrue(any(f['kind'] == 'personal-build-path' for f in result))
        self.assertTrue(all(set(f) == {'kind', 'line'} for f in result))

    def test_replacement_directory_escape_rejected(self):
        with self.assertRaises(ValueError):
            subject.replacement_bytes(self.policy, '../repo/private.txt')


if __name__ == '__main__':
    unittest.main()
