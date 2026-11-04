"""Run the real storage planner on sparse files; never open an Android device."""
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_storage
from test_install_piano import entry, fixture_gpt, install


class AndroidStorageSizeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='piano-storage-sizes-')
        cls.directory = Path(cls.work.name)
        build_storage.build(cls.directory / 'bin', 'host')
        cls.binary = cls.directory / 'bin/piano-storage'

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def write_disk(self, folder, gpt):
        disk = folder / 'disk.img'
        with disk.open('wb') as stream:
            stream.truncate(gpt.disk_bytes)
            for offset, data in self.metadata(gpt):
                stream.seek(offset)
                stream.write(data)
        return disk

    @staticmethod
    def metadata(gpt):
        return ((4096, gpt.primary), (8192, gpt.primary_entries),
                (gpt.disk_bytes - 4096, gpt.backup),
                (gpt.backup_table_lba * 4096, gpt.backup_entries))

    def assert_metadata_equal(self, disk, gpt):
        with disk.open('rb') as stream:
            for offset, data in self.metadata(gpt):
                stream.seek(offset)
                self.assertEqual(stream.read(len(data)), data)

    def run_native(self, disk, *arguments):
        return subprocess.run([str(self.binary), *map(str, arguments),
                               '--offline', str(disk), '4096'],
                              capture_output=True, text=True)

    def apply_plan(self, disk, plan, original):
        run = self.run_native(disk, 'apply', plan, plan.with_suffix('.backup'), '--execute')
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue(json.loads(run.stdout)['metadata_readback_equal'])
        with disk.open('rb') as stream:
            parts = []
            for offset, data in self.metadata(original):
                stream.seek(offset)
                parts.append(stream.read(len(data)))
        return install.parse_gpt(parts[0], parts[1], parts[2], parts[3], original.disk_bytes)

    @staticmethod
    def with_table(gpt, table):
        import zlib
        checksum = zlib.crc32(table)
        return install.parse_gpt(install._new_header(gpt.primary, checksum), bytes(table),
                                 install._new_header(gpt.backup, checksum), bytes(table),
                                 gpt.disk_bytes)

    def test_create_512_gib_preserves_other_entries_on_one_tib_disk(self):
        with tempfile.TemporaryDirectory(dir=self.directory) as folder:
            folder = Path(folder)
            original = fixture_gpt(1024)
            disk = self.write_disk(folder, original)
            plan = folder / 'create.plan'
            section = 2 << 20
            run = self.run_native(disk, 'plan', 'create', 512, plan,
                                  '--userdata-section-bytes', section)
            self.assertEqual(run.returncode, 0, run.stderr)
            description = json.loads(run.stdout)
            self.assertTrue(description['requires_userdata_shrink'])
            self.assertEqual(description['target_root_bytes'], 512 << 30)
            self.assertEqual(description['userdata_target_bytes'] % section, 0)
            self.assertGreater(description['userdata_target_bytes'], 1 << 32)
            self.assert_metadata_equal(disk, original)

            updated = self.apply_plan(disk, plan, original)
            before = {row['name']: row for row in original.partitions}
            after = {row['name']: row for row in updated.partitions}
            self.assertEqual(after['sunuefi_root']['bytes'], 512 << 30)
            self.assertEqual(after['sunuefi_esp']['bytes'], 512 << 20)
            self.assertEqual(after['userdata']['bytes'], description['userdata_target_bytes'])
            self.assertEqual(after['sunuefi_esp']['first'], after['userdata']['last'] + 1)
            self.assertEqual(after['sunuefi_root']['first'], after['sunuefi_esp']['last'] + 1)
            self.assertEqual((after['userdata']['first'], after['userdata']['guid'],
                              after['userdata']['attributes']),
                             (before['userdata']['first'], before['userdata']['guid'],
                              before['userdata']['attributes']))
            userdata_offset = before['userdata']['index'] * 128
            expected_userdata = bytearray(original.primary_entries[userdata_offset:userdata_offset + 128])
            struct.pack_into('<Q', expected_userdata, 40, after['userdata']['last'])
            self.assertEqual(updated.primary_entries[userdata_offset:userdata_offset + 128],
                             bytes(expected_userdata))
            changed = {after[name]['index'] for name in ('userdata', 'sunuefi_esp', 'sunuefi_root')}
            for index in range(original.geometry['count']):
                if index not in changed:
                    span = slice(index * 128, (index + 1) * 128)
                    self.assertEqual(updated.primary_entries[span], original.primary_entries[span])

    def test_create_512_gib_refuses_insufficient_userdata_on_one_tib_disk(self):
        with tempfile.TemporaryDirectory(dir=self.directory) as folder:
            folder = Path(folder)
            gpt = fixture_gpt(1024)
            table = bytearray(gpt.primary_entries)
            userdata_start = (600 << 30) // 4096
            struct.pack_into('<Q', table, 128 + 32, userdata_start)
            struct.pack_into('<Q', table, 7 * 128 + 40, userdata_start - 1)
            original = self.with_table(gpt, table)
            disk = self.write_disk(folder, original)
            plan = folder / 'create.plan'
            run = self.run_native(disk, 'plan', 'create', 512, plan,
                                  '--userdata-section-bytes', 2 << 20)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('Android 数据分区没有足够空间可划分', run.stderr)
            self.assertFalse(plan.exists())
            self.assert_metadata_equal(disk, original)

    def test_resize_root_to_512_gib_keeps_start_and_refuses_overlap_or_disk_end(self):
        for root_start_gib, blocked in ((64, False), (64, True), (600, False)):
            with self.subTest(root_start_gib=root_start_gib, blocked=blocked), \
                    tempfile.TemporaryDirectory(dir=self.directory) as folder:
                folder = Path(folder)
                gpt = fixture_gpt(1024)
                table = bytearray(gpt.primary_entries)
                root_start = (root_start_gib << 30) // 4096
                esp_start = root_start - (512 << 20) // 4096
                struct.pack_into('<Q', table, 128 + 40, esp_start - 1)
                table[:128] = entry('sunuefi_esp', esp_start, root_start - 1,
                                    '00000000-0000-0000-0000-000000000100', install.ESP_TYPE)
                table[2 * 128:3 * 128] = entry('sunuefi_root', root_start,
                    root_start + (128 << 30) // 4096 - 1,
                    '00000000-0000-0000-0000-000000000101')
                if blocked:
                    start = root_start + (256 << 30) // 4096
                    table[3 * 128:4 * 128] = entry('keep-after-root', start, start + 511,
                                                  '00000000-0000-0000-0000-000000000102')
                original = self.with_table(gpt, table)
                disk = self.write_disk(folder, original)
                plan = folder / 'resize.plan'
                run = self.run_native(disk, 'plan', 'resize', 512, plan)
                self.assert_metadata_equal(disk, original)
                if blocked or root_start_gib == 600:
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn('相邻空闲空间' if blocked else '超出磁盘末尾', run.stderr)
                    self.assertFalse(plan.exists())
                    continue
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(json.loads(run.stdout)['target_root_bytes'], 512 << 30)
                updated = self.apply_plan(disk, plan, original)
                before = next(row for row in original.partitions if row['name'] == 'sunuefi_root')
                after = next(row for row in updated.partitions if row['name'] == 'sunuefi_root')
                self.assertEqual(after['bytes'], 512 << 30)
                self.assertEqual(after['first'], before['first'])
                expected = bytearray(original.primary_entries)
                struct.pack_into('<Q', expected, before['index'] * 128 + 40, after['last'])
                self.assertEqual(updated.primary_entries, bytes(expected))

    def test_mksh_plan_uses_native_64bit_section_bytes(self):
        shell = os.environ.get('MKSH') or shutil.which('mksh')
        if not shell:
            self.skipTest('Android-compatible mksh is needed for this regression')
        for capacity, section, choice in ((128, 2 << 20, 32),
                                          (256, 16 << 20, 64),
                                          (512, 8 << 30, 256),
                                          (1024, 2 << 20, 512)):
            with self.subTest(capacity=capacity, section=section), \
                    tempfile.TemporaryDirectory(dir=self.directory) as folder:
                folder = Path(folder)
                gpt = fixture_gpt(capacity)
                disk = self.write_disk(folder, gpt)
                module = folder / 'module'
                (module / 'bin').mkdir(parents=True)
                state = folder / 'state'
                script = (ROOT / 'android/module/storage-operations.sh').read_text()
                (module / 'storage-operations.sh').write_text(script.replace(
                    'STATE=/data/adb/piano-sunuefi/storage', 'STATE=' + shlex.quote(str(state))))
                wrappers = {
                    'piano-storage': '#!/bin/sh\nexec ' + shlex.quote(str(self.binary)) +
                        ' "$@" --offline ' + shlex.quote(str(disk)) + ' 4096\n',
                    'piano-resize-f2fs': '#!/bin/sh\nprintf \'%s\\n\' ' + shlex.quote(json.dumps({
                        'section_blocks': section // 4096, 'section_bytes': section}, separators=(',', ':'))) + '\n',
                    'getprop': '#!/bin/sh\ncase "$1" in ro.product.device) echo piano;; '
                        'ro.boot.flash.locked) echo 1;; esac\n',
                    'id': '#!/bin/sh\necho 0\n',
                }
                for name, text in wrappers.items():
                    path = module / 'bin' / name
                    path.write_text(text)
                    path.chmod(0o755)
                run = subprocess.run([shell, str(module / 'storage-operations.sh'),
                                      'plan', 'create', str(choice)], capture_output=True, text=True,
                                     env={**os.environ, 'PATH': str(module / 'bin') + ':' + os.environ['PATH']})
                self.assertEqual(run.returncode, 0, run.stderr)
                description = json.loads(run.stdout)
                self.assertTrue(description['requires_userdata_shrink'])
                self.assertEqual(description['userdata_target_bytes'] % section, 0)
                self.assertEqual(description['target_root_bytes'], choice << 30)
                facts = subprocess.check_output([str(self.binary), 'facts',
                    str(state / (description['plan_id'] + '.plan')), '--offline', str(disk), '4096'], text=True).split()
                self.assertEqual((len(facts), facts[8], facts[9]), (10, str(choice), '0'))
                self.assert_metadata_equal(disk, gpt)
