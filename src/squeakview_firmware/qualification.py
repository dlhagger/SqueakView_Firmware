"""Protocol parsing and presentation data for MouseHouse qualification."""

from __future__ import annotations

from dataclasses import dataclass, field


PROTOCOL_VERSION = "1"

CHECK_LABELS = {
    "rtc": "RTC clock",
    "left_poke": "Left poke sensor",
    "right_poke": "Right poke sensor",
    "left_drink": "Left drink sensor",
    "right_drink": "Right drink sensor",
    "pellet_blocked": "Pellet beam blocked",
    "pellet_cleared": "Pellet beam cleared",
    "led_rgbw": "NeoPixel RGBW output",
    "feed_dark_fw": "Feed with LEDs off",
    "feed_dark_motor": "Motor movement, LEDs off",
    "feed_light_fw": "Feed with indicator on",
    "feed_light_motor": "Motor movement, indicator on",
    "jam_latched": "Jam detection",
    "jam_cleared": "Jam recovery",
    "reversal": "Retry direction reversal",
    "buzzer": "Buzzer",
    "camera_cycle": "Camera session cycle",
    "camera_ttl": "Physical camera TTL",
    "house_light": "House light",
}


ACTION_TEXT = {
    "ENTER_DEVICE_ID": (
        "Identify this unit",
        "Enter the identifier that should appear in its deployment record.",
    ),
    "SET_RTC_FROM_HOST": (
        "Set the real-time clock",
        "The controller clock is invalid. Use the button below to copy this computer's time.",
    ),
    "ENTER_AND_LEAVE_LEFT_POKE": (
        "Test the left poke",
        "Break the left poke beam, then move your hand away. The GUI advances after the full sensor cycle.",
    ),
    "ENTER_AND_LEAVE_RIGHT_POKE": (
        "Test the right poke",
        "Break the right poke beam, then move your hand away. The GUI advances after the full sensor cycle.",
    ),
    "BLOCK_PELLET_BEAM": (
        "Block the pellet-well beam",
        "Place a pellet or test object in the pellet well and leave it there.",
    ),
    "CLEAR_PELLET_BEAM": (
        "Clear the pellet-well beam",
        "Remove the pellet or test object from the well.",
    ),
    "ACTIVATE_AND_RELEASE_LEFT_DRINK": (
        "Test the left drink sensor",
        "Activate the left lickometer contact, then release it completely.",
    ),
    "ACTIVATE_AND_RELEASE_RIGHT_DRINK": (
        "Test the right drink sensor",
        "Activate the right lickometer contact, then release it completely.",
    ),
    "WATCH_ALL_PIXELS_RED_GREEN_BLUE_WHITE_OFF": (
        "Watch the complete LED color sweep",
        "Confirm that the eight-pixel bar and both poke pixels show red, green, blue, white, then turn off.",
    ),
    "CONFIRM_LED_RGBW": (
        "Did every pixel show the correct colors?",
        "Check for missing pixels, swapped colors, flicker, or uneven brightness.",
    ),
    "RIGHT_POKE_TO_FEED_DARK_THEN_TRIGGER_PELLET_BEAM": (
        "Test feeding with all LEDs off",
        "Complete a right poke. Verify the motor turns while every NeoPixel stays off, then block the pellet beam.",
    ),
    "CONFIRM_FEED_DARK_MOTOR": (
        "Did the motor move with all LEDs off?",
        "Answer Yes only if you physically observed feeder movement.",
    ),
    "REMOVE_PELLET_AND_CLEAR_BEAM": (
        "Clear the pellet well",
        "Remove the delivered pellet and wait for the controller to detect a clear beam.",
    ),
    "LEFT_POKE_TO_FEED_WITH_BLUE_LIGHT_THEN_TRIGGER_PELLET_BEAM": (
        "Test feeding with an indicator on",
        "Complete a left poke. Verify the motor turns while the left pixel stays blue, then block the pellet beam.",
    ),
    "CONFIRM_FEED_LIGHT_MOTOR": (
        "Did the motor move with the blue light on?",
        "Answer Yes only if the motor and blue indicator operated together.",
    ),
    "RIGHT_POKE_THEN_LEAVE_PELLET_BEAM_CLEAR_UNTIL_JAM": (
        "Test retries and jam detection",
        "Complete a right poke, keep the pellet beam clear, and watch for motor direction reversal before the jam stops it.",
    ),
    "PHYSICALLY_CLEAR_FEEDER_THEN_SEND CLEAR_JAM": (
        "Clear the feeder jam",
        "Inspect and physically clear the mechanism, then use the Clear Jam button.",
    ),
    "CONFIRM_RETRY_REVERSAL": (
        "Did the motor reverse during retries?",
        "Confirm the physical retry direction changed before the jam was reported.",
    ),
    "CONFIRM_BUZZER": (
        "Did you hear the buzzer?",
        "A short tone should have played when this step appeared.",
    ),
    "RUN_CAMERA_START_STOP": (
        "Test the camera synchronization output",
        "Start the 30 FPS test, observe several pulses on the receiver or meter, then stop it.",
    ),
    "CONFIRM_CAMERA_TTL": (
        "Was the physical camera TTL detected?",
        "Confirm using the camera receiver, oscilloscope, or logic analyzer—not serial messages alone.",
    ),
    "CONFIRM_HOUSE_LIGHT": (
        "Is the house light operating correctly?",
        "Verify its physical state matches the current light schedule.",
    ),
    "QUALIFICATION_COMPLETE": (
        "Qualification complete",
        "The final signed-off record has been received and saved by this application.",
    ),
    "RESTART_QUALIFICATION": (
        "Qualification aborted",
        "Restart when the device is safe and ready for another complete test.",
    ),
}


CONFIRMATION_STAGES = {
    "LED_CONFIRM",
    "FEED_DARK_CONFIRM",
    "FEED_LIGHT_CONFIRM",
    "REVERSAL_CONFIRM",
    "BUZZER_CONFIRM",
    "CAMERA_CONFIRM",
    "HOUSE_LIGHT_CONFIRM",
}


def parse_fields(line: str) -> tuple[str, list[str], dict[str, str]]:
    """Split one comma-delimited firmware line into positional and keyed data."""
    parts = [part.strip() for part in line.strip().split(",")]
    kind = parts[0] if parts else ""
    positional: list[str] = []
    keyed: dict[str, str] = {}
    for part in parts[1:]:
        if "=" in part:
            key, value = part.split("=", 1)
            keyed[key] = value
        elif part:
            positional.append(part)
    return kind, positional, keyed


@dataclass
class QualificationState:
    stage: str = "DISCONNECTED"
    action: str = ""
    device_id: str = "UNSET"
    checks: dict[str, bool] = field(
        default_factory=lambda: {key: False for key in CHECK_LABELS}
    )
    final_record: dict[str, str] | None = None
    last_error: str = ""
    protocol_version: str = ""

    @property
    def title(self) -> str:
        return ACTION_TEXT.get(self.action, (self.stage.replace("_", " ").title(), ""))[0]

    @property
    def instructions(self) -> str:
        return ACTION_TEXT.get(self.action, ("", self.action.replace("_", " ").title()))[1]

    @property
    def awaiting_confirmation(self) -> bool:
        return self.stage in CONFIRMATION_STAGES

    def reset(self) -> None:
        self.stage = "DISCONNECTED"
        self.action = ""
        self.device_id = "UNSET"
        self.checks = {key: False for key in CHECK_LABELS}
        self.final_record = None
        self.last_error = ""
        self.protocol_version = ""

    def apply_line(self, line: str) -> str:
        """Apply a firmware line and return a high-level event name."""
        kind, positional, keyed = parse_fields(line)
        if kind == "SETUP_STAGE" and positional:
            self.stage = positional[0]
            return "stage"
        if kind == "SETUP_PROMPT":
            self.stage = keyed.get("stage", self.stage)
            self.action = keyed.get("action", "")
            return "prompt"
        if kind == "SETUP_CHECKLIST":
            self.device_id = keyed.get("device", self.device_id)
            self.stage = keyed.get("stage", self.stage)
            for key in self.checks:
                if key in keyed:
                    self.checks[key] = keyed[key] == "1"
            return "checklist"
        if kind == "SETUP_STEP_RESULT":
            return "step_result"
        if kind == "SETUP_QUALIFICATION_RECORD":
            self.final_record = keyed
            self.device_id = keyed.get("device", self.device_id)
            return "record"
        if kind == "ACK_TEST_DEVICE" and positional:
            self.device_id = positional[0]
            return "device"
        if kind == "NACK":
            self.last_error = line
            return "error"
        if kind == "SETUP_DEBUG_READY":
            return "ready"
        if kind == "SETUP_PROTOCOL":
            self.protocol_version = keyed.get("version", "")
            return "protocol"
        return "line"
