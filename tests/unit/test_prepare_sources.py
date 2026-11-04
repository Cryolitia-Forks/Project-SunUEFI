"""Check scoped source pins against real local Git repositories, without network."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_sources as sources


class PrepareSourcesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='prepare-sources-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull,
                    'GIT_CONFIG_NOSYSTEM': '1'}
        self.nested = ('upstream/Mu-Silicium/Mu_Basecore',
                       'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Library/BaseFdtLib/libfdt',
                       'upstream/simple-init/libs/freetype')
        self.pins = {}
        for relative in (*sources.UEFI_TOP, *self.nested):
            path = self.root / relative
            path.mkdir(parents=True)
            self.git(path, 'init', '-q')
            self.git(path, 'config', 'user.name', 'Source Test')
            self.git(path, 'config', 'user.email', 'source@example.invalid')
            (path / 'source.txt').write_text(relative + '\n')
            self.git(path, 'add', 'source.txt')
            self.git(path, 'commit', '-qm', 'test: initial source')
            self.pins[relative] = self.git(path, 'rev-parse', 'HEAD')
        lock = {name.removeprefix('upstream/'): {'commit': pin}
                for name, pin in self.pins.items() if name != 'upstream/linux-piano'}
        # The distribution checkout is intentionally absent in a UEFI-only clone.
        lock['debian-piano-current'] = {'commit': self.pins['upstream/linux-piano']}
        (self.root / 'sources.lock.json').write_text(json.dumps(lock))
        (self.root / 'linux').mkdir()
        (self.root / 'linux/kernel-profiles.json').write_text(json.dumps({
            'profiles': {'stable': {'commit': self.pins['upstream/linux-piano']}}}))

    def git(self, path, *args):
        return subprocess.check_output(
            ['git', '-C', str(path), '-c', 'commit.gpgsign=false', *args],
            text=True, env=self.env, stderr=subprocess.PIPE).strip()

    def test_uefi_check_accepts_scoped_checkout_without_distribution_sources(self):
        self.assertEqual(sources.check(self.root, sources.UEFI_TOP), len(self.pins))
        with self.assertRaisesRegex(ValueError, 'debian-piano-current: missing checkout'):
            sources.check(self.root)

    def test_default_check_preserves_all_locked_sources(self):
        path = self.root / 'sources.lock.json'
        lock = json.loads(path.read_text())
        lock['additional-source'] = {'commit': self.pins['upstream/linux-piano']}
        path.write_text(json.dumps(lock))
        with self.assertRaisesRegex(ValueError, 'additional-source: missing checkout'):
            sources.check(self.root)
        self.assertEqual(sources.check(self.root, sources.UEFI_TOP), len(self.pins))

    def test_missing_or_changed_nested_checkout_is_rejected(self):
        for relative in self.nested:
            with self.subTest(relative=relative):
                path = self.root / relative
                # An existing child directory without its own Git checkout would
                # otherwise resolve the parent repository through rev-parse.
                gitdir = path / '.git'
                saved = path / '.git-saved'
                gitdir.rename(saved)
                try:
                    with self.assertRaisesRegex(ValueError, relative + ': expected'):
                        sources.check(self.root, sources.UEFI_TOP)
                finally:
                    saved.rename(gitdir)
                (path / 'source.txt').write_text('changed nested revision\n')
                self.git(path, 'add', 'source.txt')
                self.git(path, 'commit', '-qm', 'test: changed nested source')
                with self.assertRaisesRegex(ValueError, relative + ': expected'):
                    sources.check(self.root, sources.UEFI_TOP)
                self.git(path, 'checkout', '-q', '--detach', self.pins[relative])
        self.assertEqual(sources.check(self.root, sources.UEFI_TOP), len(self.pins))

    def test_cli_uefi_scope_reaches_read_only_source_check(self):
        tools = self.root / 'tools'
        tools.mkdir()
        script = tools / 'prepare_sources.py'
        shutil.copy2(ROOT / 'tools/prepare_sources.py', script)
        env = {**self.env, 'SUNUEFI_BUILD_TARGET': 'uefi'}
        result = subprocess.run([sys.executable, str(script), '--check'],
                                env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f'{len(self.pins)} fixed source checkouts verified', result.stdout)
        env.pop('SUNUEFI_BUILD_TARGET')
        result = subprocess.run([sys.executable, str(script), '--check'],
                                env=env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('debian-piano-current: missing checkout', result.stderr)


class PrepareSourcePinTests(unittest.TestCase):
    def test_uefi_scope_preserves_all_locked_firmware_dependencies(self):
        lock = json.loads((ROOT / 'sources.lock.json').read_text())
        scoped = sources.expected(ROOT, sources.UEFI_TOP)
        required = {'upstream/linux-piano', 'upstream/dtc',
                    'upstream/Mu-Silicium', 'upstream/simple-init'}
        required.update('upstream/' + name for name in lock
                        if name.startswith(('Mu-Silicium/', 'simple-init/')))
        self.assertEqual(set(scoped), required)
        for relative in required - {'upstream/linux-piano'}:
            self.assertEqual(scoped[relative], lock[relative.removeprefix('upstream/')]['commit'])
        for parent, children in sources.NESTED.items():
            for child in children:
                self.assertIn(parent + '/' + child, scoped)
        self.assertNotIn('upstream/debian-piano-current', scoped)
        self.assertNotIn('upstream/piano-firmware-current', scoped)


if __name__ == '__main__':
    unittest.main()
