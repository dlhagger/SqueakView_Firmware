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
        source = (ROOT / "src/MouseHouse.cpp").read_text()
        methods = []
        for signature in (
            "void MouseHouse::startSession(", "void MouseHouse::stopSession()",
            "void MouseHouse::update()", "void MouseHouse::drainCameraEvents()",
            "bool MouseHouse::cameraTriggersEnabled() const",
        ):
            start = source.index(signature)
            opening = source.index("{", start)
            depth = 1
            closing = opening + 1
            while depth:
                depth += (source[closing] == "{") - (source[closing] == "}")
                closing += 1
            methods.append(source[start:closing])
        (cls.build / "session_methods.inc").write_text("\n".join(methods))
        methods = []
        for signature in (
            "void MouseHouse::handleBottleCommand(",
            "void MouseHouse::serviceBottleCalibration()",
            "void MouseHouse::failBottleCalibration(",
            "void MouseHouse::queueBottleState()", "void MouseHouse::muteDrinkEvents(",
            "void MouseHouse::stopSession()",
            "void MouseHouse::beginLickRecovery(", "void MouseHouse::serviceLickRecovery()",
            "void MouseHouse::reportLickRecovery(", "void MouseHouse::reportLickRecoveryDiagnostic(",
            "void MouseHouse::cancelLickRecovery()",
            "void MouseHouse::checkMPR121Health()", "void MouseHouse::setLickRecoveryTouchTimeoutMs(",
            "void MouseHouse::updateLeftDrink()", "void MouseHouse::updateRightDrink()",
            "void MouseHouse::updateTimedBinaryEvent(",
        ):
            start = source.index(signature)
            opening = source.index("{", start)
            depth = 1
            closing = opening + 1
            while depth:
                depth += (source[closing] == "{") - (source[closing] == "}")
                closing += 1
            methods.append(source[start:closing])
        (cls.build / "calibration_methods.inc").write_text("\n".join(methods))
        for name in ("replay", "qualification", "go_nogo", "bottle_swap", "rtc", "behavior_session", "bottle_calibration"):
            command = [
                "g++", "-std=c++17", "-Wall", "-Wextra",
                f"-I{NATIVE}", f"-I{ROOT / 'src'}", f"-I{cls.build}",
                str(NATIVE / f"{name}.cpp"),
            ]
            if name == "replay":
                command.append(str(ROOT / "src/SerialTransport.cpp"))
            if name == "rtc":
                command.insert(1, f"-I{NATIVE / 'rtc'}")
            result = subprocess.run(command + ["-o", str(cls.build / name)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)

    def run_native(self, name, argument):
        result = subprocess.run([str(self.build / name), str(argument)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        return result.stdout

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

    def test_bottle_swap_retries_calibration_and_counter_boundaries(self):
        self.run_native("bottle_swap", 0)

    def test_rtc_detection_dispatch_and_fail_closed_without_register_writes(self):
        for scenario in range(14):
            with self.subTest(scenario=scenario):
                self.run_native("rtc", scenario)

    def test_production_session_modes_keep_behavior_running_without_camera_records(self):
        self.run_native("behavior_session", 0)

    def test_go_nogo_only_emits_legacy_start_ack_for_triggered_sessions(self):
        for scenario in (2, 3):
            with self.subTest(scenario=scenario):
                self.run_native("go_nogo", scenario)

    def test_production_calibration_register_failures_readiness_and_manual_retry(self):
        self.run_native("bottle_calibration", 0)
