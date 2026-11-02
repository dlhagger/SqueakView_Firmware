import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from squeakview_firmware.clock_transport import ClockTransport
from squeakview_firmware.clock_sync import collect
from squeakview_firmware import controller_protocol_v2 as protocol
from test_protocol_v2 import build_frame


class Controller:
    """Independent firmware wire encoder, byte-at-a-time USB reads."""
    def __init__(self, *, v2=True):
        self.v2 = v2
        self.pending = bytearray()
        self.writes = []
        self.sequence = 100
        self.boot = 77
        self.corrupt = False
        self.session = 0
        self.on_ack = lambda sequence: None

    def wire(self, text, sequence=None):
        if not self.v2:
            return (text + "\n").encode()
        result = build_frame(text.encode(), sequence=sequence or self.sequence,
                             boot=self.boot, session=self.session, message_type=2)
        if self.corrupt:
            result = result[:-2] + bytes([result[-2] ^ 1]) + result[-1:]
        return result

    def write(self, value):
        text = value.decode().strip()
        self.writes.append(text)
        if text.startswith("TIME_SYNC,"):
            _, request, sent = text.split(",")
            self.pending.extend(self.wire(f"CLOCK_SYNC,{request},{sent},100,104,1700000000000000,RTC_VALID"))
            self.sequence += 1
        elif text.startswith("ACK_EVENTS,"):
            self.on_ack(int(text.split(",")[2]))
        return len(value)

    def read(self, size):
        result = bytes(self.pending[:size])
        del self.pending[:size]
        return result


class ClockTransportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "clock.jsonl"

    def tearDown(self):
        self.temporary.cleanup()

    def test_cold_boot_remains_legacy_and_collects_without_negotiation(self):
        controller = Controller(v2=False)
        controller.pending.extend(b"MOUSEHOUSE_BUILD,test\n")
        with ClockTransport(controller, self.path) as transport:
            self.assertEqual(collect(transport, 7)["sequence"], 7)
        self.assertFalse(self.path.exists())
        self.assertTrue(all(command.startswith("TIME_SYNC,") for command in controller.writes))

    def test_v2_reconnect_ignores_stale_session_and_fsyncs_before_each_ack(self):
        controller = Controller()
        controller.pending.extend(b"old partial frame\0")
        controller.pending.extend(build_frame(b"CLOCK_SYNC,0,1,1,1,1,RTC_VALID", sequence=99, boot=77, session=7))
        syncs = []
        real_fsync = protocol.os.fsync
        def sync(fd):
            real_fsync(fd)
            syncs.append(fd)
        def ack(sequence):
            self.assertTrue(syncs)
            rows = [json.loads(line) for line in self.path.read_text().splitlines()]
            self.assertEqual(rows[-1]["sequence"], sequence)
            self.assertEqual(rows[0]["sequence"], 100)
            syncs.clear()
        controller.on_ack = ack
        with patch.object(protocol.os, "fsync", side_effect=sync):
            with ClockTransport(controller, self.path) as transport:
                for sequence in range(70):  # Exceeds the firmware RAM queue capacity.
                    self.assertEqual(collect(transport, sequence)["sequence"], sequence)
        self.assertEqual(len(self.path.read_text().splitlines()), 71)
        self.assertFalse(list(self.path.parent.glob("*.sqlite3")))

    def test_crc_failure_is_never_acknowledged(self):
        controller = Controller()
        with ClockTransport(controller, self.path) as transport:
            controller.corrupt = True
            with self.assertRaisesRegex(ValueError, "CRC"):
                collect(transport, 1)
        self.assertEqual([line for line in controller.writes if line.startswith("ACK_EVENTS")], ["ACK_EVENTS,77,100"])

    def test_reboot_or_active_session_is_rejected_before_ack(self):
        for name, value, error in (("boot", 88, "reboot"), ("session", 1, "idle")):
            with self.subTest(name=name):
                controller = Controller()
                path = self.path.with_name(name + ".jsonl")
                with ClockTransport(controller, path) as transport:
                    setattr(controller, name, value)
                    with self.assertRaisesRegex(RuntimeError, error):
                        collect(transport, 1)
                self.assertEqual(len(path.read_text().splitlines()), 1)

    def test_gap_replay_retains_original_receipt_times_and_contiguous_ack(self):
        controller = Controller()
        original_write = controller.write
        def write(value):
            result = original_write(value)
            if value.startswith(b"RESEND_EVENTS,"):
                controller.pending.extend(controller.wire("ACK_OLD_IDLE_COMMAND", 101))
            return result
        controller.write = write
        with ClockTransport(controller, self.path) as transport:
            controller.sequence = 102
            sample = collect(transport, 9)
            self.assertEqual(sample["sequence"], 9)
        rows = [json.loads(line) for line in self.path.read_text().splitlines()]
        self.assertEqual([row["sequence"] for row in rows], [100, 102, 101])
        self.assertIn("RESEND_EVENTS,77,101", controller.writes)
        self.assertEqual(controller.writes[-1], "ACK_EVENTS,77,102")
        self.assertEqual(sample["jetson_receive_ns"], rows[1]["host_unix_ns"])

    def test_disk_failure_does_not_acknowledge_response(self):
        controller = Controller()
        with ClockTransport(controller, self.path) as transport:
            with patch.object(protocol.os, "fsync", side_effect=OSError("disk full")):
                with self.assertRaisesRegex(OSError, "disk full"):
                    collect(transport, 1)
        self.assertNotIn("ACK_EVENTS,77,101", controller.writes)

    def test_independent_wire_encoder_matches_vendored_codec(self):
        for sequence in (1, 2**63, 2**64 - 1):
            frame = protocol.decode_frame(build_frame(b"ACK_FEED", sequence=sequence)[:-1])
            self.assertEqual(frame.sequence, sequence)
            self.assertEqual(frame.text, "ACK_FEED")


if __name__ == "__main__":
    unittest.main()
