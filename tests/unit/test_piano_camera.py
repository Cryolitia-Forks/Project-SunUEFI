"""Check real daemon geometry and format handoff without opening camera devices."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PianoCameraTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('Native C compiler unavailable')
        temporary = tempfile.TemporaryDirectory(prefix='piano-camera-')
        cls.addClassCleanup(temporary.cleanup)
        work = Path(temporary.name)
        source = ROOT / 'linux/userspace/camera/piano-camerad.c'
        cls.binary = work / 'camera-check'
        subprocess.run([
            *compiler, '-O2', f'-DPIANO_CAMERAD_SOURCE="{source}"',
            str(ROOT / 'tests/native/test_piano_camera.c'), '-lm',
            '-o', str(cls.binary),
        ], check=True)

    def check_camera(self, scenario):
        result = subprocess.run([str(self.binary), scenario],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_aspect_preserving_scaler_and_centered_crop(self):
        self.check_camera('geometry')

    def test_format_handoff_releases_output_before_full_black_frame(self):
        self.check_camera('reconfigure')

    def test_sensor_modes_do_not_advertise_unsupported_rates(self):
        self.check_camera('modes')

    def test_frame_period_uses_vt_pixel_rate_and_blanking(self):
        self.check_camera('timing')

    def test_front_full_resolution_uses_fixed_native_timing(self):
        self.check_camera('front-timing')

    def test_sixty_fps_handoff_selects_the_dedicated_sensor_mode(self):
        self.check_camera('reconfigure60')

    def test_failed_streamoff_retains_capture_buffers(self):
        self.check_camera('stop-error')


if __name__ == '__main__':
    unittest.main()
