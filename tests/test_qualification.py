import unittest
from pathlib import Path
from types import SimpleNamespace

from squeakview_firmware.qualification import (
    ACTION_TEXT,
    PROTOCOL_VERSION,
    QualificationState,
    parse_fields,
)
from squeakview_firmware.qualification_gui import is_mousehouse_serial_port


class QualificationProtocolTests(unittest.TestCase):
    def test_only_usb_cdc_controller_ports_are_offered(self):
        self.assertTrue(
            is_mousehouse_serial_port(SimpleNamespace(device="/dev/ttyACM0"))
        )
        self.assertFalse(
            is_mousehouse_serial_port(SimpleNamespace(device="/dev/ttyAMA0"))
        )
        self.assertFalse(
            is_mousehouse_serial_port(SimpleNamespace(device="/dev/ttyUSB0"))
        )

    def test_action_codes_are_safe_for_comma_delimited_protocol(self):
        self.assertTrue(ACTION_TEXT)
        self.assertTrue(all("," not in action for action in ACTION_TEXT))

    def test_gui_action_codes_match_setup_debug_firmware(self):
        firmware = (
            Path(__file__).resolve().parents[1]
            / "examples"
            / "setup_debug"
            / "setup_debug.ino"
        ).read_text(encoding="utf-8")
        for action in ACTION_TEXT:
            self.assertIn(f'Serial.println("{action}")', firmware)

    def test_setup_requires_host_clock_verification(self):
        firmware = (
            Path(__file__).resolve().parents[1]
            / "examples"
            / "setup_debug"
            / "setup_debug.ino"
        ).read_text(encoding="utf-8")
        self.assertIn('strcmp(command, "TEST,RTC_VERIFIED")', firmware)
        self.assertIn('setStage(STAGE_RTC);', firmware)
        self.assertNotIn('mh.rtcValid() ? STAGE_LEFT_POKE : STAGE_RTC', firmware)

    def test_gui_has_bounded_firmware_handshake(self):
        gui = (
            Path(__file__).resolve().parents[1]
            / "src"
            / "squeakview_firmware"
            / "qualification_gui.py"
        ).read_text(encoding="utf-8")
        self.assertIn("HANDSHAKE_TIMEOUT_MS", gui)
        self.assertIn("Firmware handshake timed out", gui)

    def test_protocol_handshake_is_recorded(self):
        state = QualificationState()
        event = state.apply_line(
            f"SETUP_PROTOCOL,version={PROTOCOL_VERSION},mode=GUIDED_QUALIFICATION"
        )
        self.assertEqual(event, "protocol")
        self.assertEqual(state.protocol_version, PROTOCOL_VERSION)

    def test_parse_fields_separates_positional_and_keyed_values(self):
        kind, positional, keyed = parse_fields(
            "SETUP_STEP_RESULT,step=LEFT_POKE,result=PASS"
        )
        self.assertEqual(kind, "SETUP_STEP_RESULT")
        self.assertEqual(positional, [])
        self.assertEqual(keyed, {"step": "LEFT_POKE", "result": "PASS"})

    def test_prompt_updates_presentation(self):
        state = QualificationState()
        event = state.apply_line(
            "SETUP_PROMPT,stage=LEFT_POKE,action=ENTER_AND_LEAVE_LEFT_POKE"
        )
        self.assertEqual(event, "prompt")
        self.assertEqual(state.stage, "LEFT_POKE")
        self.assertEqual(state.title, "Test the left poke")
        self.assertIn("GUI advances", state.instructions)

    def test_checklist_updates_boolean_progress(self):
        state = QualificationState()
        state.apply_line(
            "SETUP_CHECKLIST,device=MH-012,stage=RIGHT_POKE,rtc=1,"
            "left_poke=1,right_poke=0,result=INCOMPLETE"
        )
        self.assertEqual(state.device_id, "MH-012")
        self.assertTrue(state.checks["rtc"])
        self.assertTrue(state.checks["left_poke"])
        self.assertFalse(state.checks["right_poke"])

    def test_final_record_is_retained(self):
        state = QualificationState()
        event = state.apply_line(
            "SETUP_QUALIFICATION_RECORD,device=MH-012,result=PASS,"
            "failed_attempts=1,storage=SERIAL_ONLY,sd_copy=TODO"
        )
        self.assertEqual(event, "record")
        self.assertEqual(state.final_record["result"], "PASS")
        self.assertEqual(state.final_record["sd_copy"], "TODO")

    def test_jam_state_is_available_outside_dedicated_jam_stage(self):
        state = QualificationState(stage="FEED_DARK")
        self.assertEqual(
            state.apply_line(
                "FEED_JAM,123,456,nan,1,69420,69420,69420,Eligible,"
                "Pellet did not trigger sensor"
            ),
            "jam",
        )
        self.assertTrue(state.feeder_jammed)
        self.assertEqual(
            state.apply_line("DEBUG_EVENT,FEED_JAM_CLEARED"), "jam_cleared"
        )
        self.assertFalse(state.feeder_jammed)
        self.assertEqual(state.stage, "FEED_DARK")

    def test_feed_jammed_nack_activates_recovery_control_state(self):
        state = QualificationState(stage="FEED_LIGHT")
        self.assertEqual(state.apply_line("NACK,FEED,JAMMED"), "error")
        self.assertTrue(state.feeder_jammed)

    def test_clear_jam_ack_clears_latch_without_changing_stage(self):
        state = QualificationState(stage="FEED_DARK", feeder_jammed=True)
        self.assertEqual(state.apply_line("ACK_CLEAR_JAM"), "jam_cleared")
        self.assertFalse(state.feeder_jammed)
        self.assertEqual(state.stage, "FEED_DARK")

    def test_camera_events_produce_controller_rate_without_changing_stage(self):
        state = QualificationState(stage="CAMERA_CYCLE")
        self.assertEqual(
            state.apply_line(
                "ACK_START,1000000,500000,nan,30,33333,69420,69420,Eligible,nan"
            ),
            "camera_started",
        )
        state.apply_line(
            "CAMERA_HIGH,1033333,533333,nan,1,69420,69420,69420,Eligible,nan"
        )
        state.apply_line(
            "CAMERA_HIGH,2033303,1533303,nan,31,69420,69420,69420,Eligible,nan"
        )
        self.assertTrue(state.camera_running)
        self.assertEqual(state.camera_trigger_count, 31)
        self.assertAlmostEqual(state.camera_elapsed_seconds, 0.99997, places=5)
        self.assertAlmostEqual(state.camera_rate_hz or 0, 30.0009, places=3)
        self.assertEqual(state.stage, "CAMERA_CYCLE")

    def test_already_running_nack_recovers_visible_running_state(self):
        state = QualificationState(stage="CAMERA_CYCLE")
        self.assertEqual(state.apply_line("NACK,START,ALREADY_RUNNING"), "error")
        self.assertTrue(state.camera_running)

    def test_camera_stop_retains_final_count_for_summary(self):
        state = QualificationState(stage="CAMERA_CYCLE", camera_running=True)
        state.observe_camera_trigger(10, 1_000_000)
        state.observe_camera_trigger(40, 2_000_000)
        self.assertEqual(
            state.apply_line(
                "ACK_STOP,3000000,2500000,nan,45,69420,69420,69420,Eligible,nan"
            ),
            "camera_stopped",
        )
        self.assertFalse(state.camera_running)
        self.assertEqual(state.camera_trigger_count, 45)


if __name__ == "__main__":
    unittest.main()
