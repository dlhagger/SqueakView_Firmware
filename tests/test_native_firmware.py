"""Run production C++ transport and task code against host hardware boundaries."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_protocol_v2 import validate_frame

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "tests/native"


@unittest.skipUnless(shutil.which("g++"), "native regression tests require g++")
class NativeFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.build = Path(cls.temporary.name)
        for name in ("replay", "qualification", "go_nogo"):
            command = [
                "g++", "-std=c++17", "-Wall", "-Wextra",
                f"-I{NATIVE}", f"-I{ROOT / 'src'}", str(NATIVE / f"{name}.cpp"),
            ]
            if name == "replay":
                command.append(str(ROOT / "src/SerialTransport.cpp"))
            result = subprocess.run(command + ["-o", str(cls.build / name)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)

    def run_native(self, name, argument):
        return subprocess.run([str(self.build / name), str(argument)], check=True,
                              capture_output=True).stdout

    def test_ack_and_slot_reuse_during_partial_replay(self):
        for ack in (0, 1, 32, 64):
            with self.subTest(ack=ack):
                output = self.run_native("replay", ack)
                self.assertTrue(output.endswith(b"\x00"))
                frames = [validate_frame(part + b"\x00")
                          for part in output[:-1].split(b"\x00")]
                replayed = [header[5] for header, _ in frames if header[2] & 2]
                new = [header[5] for header, _ in frames if not header[2] & 2]
                self.assertEqual(replayed, [1] + list(range(max(2, ack + 1), 65)))
                self.assertEqual(new, list(range(65, 65 + ack)))

    def test_abort_cancels_delayed_and_active_qualification_tests(self):
        for scenario in range(5):
            with self.subTest(scenario=scenario):
                self.run_native("qualification", scenario)

    def test_jam_blocks_trials_and_clear_jam_restores_eligibility(self):
        self.run_native("go_nogo", 0)

    def test_rejected_feed_does_not_advance_training(self):
        self.run_native("go_nogo", 1)
