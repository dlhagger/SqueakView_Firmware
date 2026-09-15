import unittest
from pathlib import Path

from squeakview_firmware.qualification import (
    ACTION_TEXT,
    PROTOCOL_VERSION,
    QualificationState,
    parse_fields,
)


class QualificationProtocolTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
