"""Tests for the full-OTA partition extraction used by the installer."""

import hashlib
import subprocess
import struct
import sys
import tempfile
import unittest
from pathlib import Path


EXTRACTOR = Path(__file__).resolve().parents[1] / "tools" / "payload_extract.py"


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def number(field, value):
    return varint(field << 3) + varint(value)


def message(field, value):
    return varint((field << 3) | 2) + varint(len(value)) + value


def make_payload(path, data, expected_hash):
    # A small full OTA with a single REPLACE operation for a single block.
    extent = number(1, 0) + number(2, 1)
    operation = number(1, 0) + number(2, 0) + number(3, len(data)) + message(6, extent)
    info = number(1, len(data)) + message(2, expected_hash)
    partition = message(1, b"system") + message(7, info) + message(8, operation)
    manifest = number(3, len(data)) + message(13, partition)
    path.write_bytes(struct.pack(">4sQQI", b"CrAU", 2, len(manifest), 0) + manifest + data)


class PayloadExtractionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.payload = self.root / "payload.bin"
        self.out = self.root / "output"
        self.data = b"test system partition"

    def run_extractor(self, *partitions):
        return subprocess.run(
            [sys.executable, str(EXTRACTOR), str(self.payload), str(self.out), *partitions],
            capture_output=True, text=True, check=False,
        )

    def test_valid_partition_is_written_and_verified(self):
        make_payload(self.payload, self.data, hashlib.sha256(self.data).digest())
        result = self.run_extractor("system")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.out / "system.img").read_bytes(), self.data)
        self.assertIn("OK", result.stdout)

    def test_corrupted_partition_fails(self):
        make_payload(self.payload, self.data, hashlib.sha256(b"different").digest())
        result = self.run_extractor("system")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("HASH MISMATCH", result.stderr)

    def test_missing_requested_partition_fails(self):
        make_payload(self.payload, self.data, hashlib.sha256(self.data).digest())
        result = self.run_extractor("modem")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("partitions not found: modem", result.stderr)


if __name__ == "__main__":
    unittest.main()
