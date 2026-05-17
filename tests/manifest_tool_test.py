"""Tests for scripts/manifest_tool.py.

Generates a tiny .jp2 with opj_compress, parses it, asserts fields.
Skipped if opj_compress is unavailable.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import manifest_tool  # noqa: E402


@unittest.skipUnless(shutil.which("opj_compress"), "opj_compress not on PATH")
class ManifestToolTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="manifest_test_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _make_jp2(self, name: str, w: int, h: int, lossless: bool,
                  tile: int | None, prog: str | None) -> Path:
        src = self.tmp / "src.pgm"
        with open(src, "wb") as f:
            f.write(f"P5\n{w} {h}\n255\n".encode())
            f.write(bytes((i * 7) & 0xFF for i in range(w * h)))
        out = self.tmp / name
        cmd = ["opj_compress", "-i", str(src), "-o", str(out)]
        if lossless:
            cmd += ["-r", "1"]   # reversible 5-3 (lossless); -I selects irreversible 9-7 (lossy)
        else:
            cmd += ["-r", "20"]
        if tile:
            cmd += ["-t", f"{tile},{tile}"]
        if prog:
            cmd += ["-p", prog]
        subprocess.run(cmd, check=True, capture_output=True)
        return out

    def test_lossless_single_tile(self):
        p = self._make_jp2("a.jp2", 64, 48, lossless=True, tile=None, prog=None)
        info = manifest_tool.parse_file(p)
        self.assertEqual(info["width"], 64)
        self.assertEqual(info["height"], 48)
        self.assertEqual(info["components"], 1)
        self.assertEqual(info["bit_depth"], 8)
        self.assertTrue(info["lossless"])
        self.assertEqual(info["container"], "jp2")
        self.assertEqual(info["tile_w"], 64)
        self.assertEqual(info["tile_h"], 48)

    def test_lossy_tiled_rlcp(self):
        p = self._make_jp2("b.jp2", 128, 128, lossless=False, tile=32, prog="RLCP")
        info = manifest_tool.parse_file(p)
        self.assertEqual(info["tile_w"], 32)
        self.assertEqual(info["tile_h"], 32)
        self.assertFalse(info["lossless"])
        self.assertEqual(info["progression"], "RLCP")

    def test_writes_manifest_json(self):
        p1 = self._make_jp2("x.jp2", 32, 32, lossless=True, tile=None, prog=None)
        out_json = self.tmp / "manifest.json"
        manifest_tool.write_manifest([p1], self.tmp, out_json, bucket_of=lambda _: "user")
        data = json.loads(out_json.read_text())
        self.assertEqual(len(data["files"]), 1)
        f0 = data["files"][0]
        self.assertEqual(f0["bucket"], "user")
        self.assertEqual(len(f0["sha256"]), 64)
        self.assertEqual(f0["width"], 32)


if __name__ == "__main__":
    unittest.main()
