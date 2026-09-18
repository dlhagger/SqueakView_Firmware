import binascii
import pathlib
import struct
import unittest


ROOT = pathlib.Path(__file__).parents[1]
VERSION = 2
HEADER = struct.Struct("<BBHQIQQH")
MAX_PAYLOAD = 384


def cobs_encode(data: bytes) -> bytes:
    output = bytearray(b"\x00")
    code_index = 0
    code = 1
    for value in data:
        if value == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(value)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    output.append(0)
    return bytes(output)


def cobs_decode(frame: bytes) -> bytes:
    if not frame or frame[-1] != 0:
        raise ValueError("missing delimiter")
    encoded = frame[:-1]
    output = bytearray()
    index = 0
    while index < len(encoded):
        code = encoded[index]
        if code == 0:
            raise ValueError("zero inside COBS body")
        index += 1
        end = index + code - 1
        if end > len(encoded):
            raise ValueError("COBS run exceeds frame")
        output.extend(encoded[index:end])
        index = end
        if code != 0xFF and index < len(encoded):
            output.append(0)
    return bytes(output)


def build_frame(payload: bytes, *, sequence=1, boot=2, session=3,
                message_type=1, flags=1, timestamp=4) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("oversized payload")
    decoded = HEADER.pack(VERSION, message_type, flags, boot, session,
                          sequence, timestamp, len(payload)) + payload
    decoded += struct.pack("<I", binascii.crc32(decoded) & 0xFFFFFFFF)
    return cobs_encode(decoded)


def validate_frame(frame: bytes) -> tuple:
    decoded = cobs_decode(frame)
    if len(decoded) < HEADER.size + 4:
        raise ValueError("short frame")
    body, received_crc = decoded[:-4], struct.unpack("<I", decoded[-4:])[0]
    if binascii.crc32(body) & 0xFFFFFFFF != received_crc:
        raise ValueError("CRC mismatch")
    header = HEADER.unpack(body[:HEADER.size])
    payload = body[HEADER.size:]
    if header[-1] != len(payload) or len(payload) > MAX_PAYLOAD:
        raise ValueError("invalid payload length")
    return header, payload


class FakeEndpoint:
    def __init__(self, capacities, short_write=False, zero_writes=0):
        self.capacities = iter(capacities)
        self.short_write = short_write
        self.zero_writes = zero_writes
        self.output = bytearray()

    def service(self, frame, offset, budget=32):
        capacity = next(self.capacities)
        requested = min(capacity, budget, len(frame) - offset)
        if self.zero_writes and requested:
            self.zero_writes -= 1
            accepted = 0
        elif self.short_write and requested > 1:
            accepted = requested - 1
        else:
            accepted = requested
        self.output.extend(frame[offset:offset + accepted])
        return offset + accepted


class ReliableQueueModel:
    def __init__(self, boot=77, capacity=4):
        self.boot = boot
        self.capacity = capacity
        self.next_sequence = 1
        self.records = []
        self.highest_transmitted = 0
        self.overflow = None

    def enqueue(self, event_type, timestamp=0):
        if len(self.records) == self.capacity:
            if self.overflow is None:
                self.overflow = (event_type, timestamp)
            return False
        self.records.append([self.next_sequence, False])
        self.next_sequence += 1
        return True

    def transmit_next(self):
        for record in self.records:
            if not record[1]:
                record[1] = True
                self.highest_transmitted = record[0]
                return record[0]
        return None

    def acknowledge(self, boot, sequence):
        if boot != self.boot or sequence > self.highest_transmitted:
            return False
        self.records = [record for record in self.records if record[0] > sequence]
        return True

    def resend(self, boot, sequence):
        if boot != self.boot:
            return "wrong_boot"
        if not self.records or sequence < self.records[0][0]:
            return "too_old"
        if sequence > self.highest_transmitted:
            return "not_emitted"
        return [record[0] for record in self.records
                if record[1] and record[0] >= sequence]


class FramingTests(unittest.TestCase):
    def test_crc32_known_vector(self):
        self.assertEqual(binascii.crc32(b"123456789") & 0xFFFFFFFF, 0xCBF43926)

    def test_cobs_round_trip_with_zeroes(self):
        value = bytes(range(256)) + b"\x00end"
        self.assertEqual(cobs_decode(cobs_encode(value)), value)

    def test_malformed_cobs_rejected(self):
        with self.assertRaises(ValueError):
            cobs_decode(b"\x05abc\x00")

    def test_crc_success_and_failure(self):
        frame = build_frame(b"POKE_START")
        self.assertEqual(validate_frame(frame)[1], b"POKE_START")
        damaged = bytearray(frame)
        damaged[5] ^= 1
        with self.assertRaises(ValueError):
            validate_frame(bytes(damaged))

    def test_maximum_payload(self):
        payload = bytes(index & 0xFF for index in range(MAX_PAYLOAD))
        self.assertEqual(validate_frame(build_frame(payload))[1], payload)

    def test_oversized_payload_rejected(self):
        with self.assertRaises(ValueError):
            build_frame(bytes(MAX_PAYLOAD + 1))


class IncrementalOutputTests(unittest.TestCase):
    def test_zero_capacity_does_not_advance(self):
        endpoint = FakeEndpoint([0])
        self.assertEqual(endpoint.service(b"abcdef", 0), 0)

    def test_one_byte_service_and_reconnect_preserve_frame(self):
        frame = build_frame(b"event")
        endpoint = FakeEndpoint([1] * len(frame))
        offset = 0
        while offset < len(frame):
            offset = endpoint.service(frame, offset)
        self.assertEqual(bytes(endpoint.output), frame)

    def test_short_writes_resume_unsent_suffix(self):
        frame = build_frame(b"partial write")
        endpoint = FakeEndpoint([5] * len(frame), short_write=True)
        offset = 0
        calls = 0
        while offset < len(frame):
            offset = endpoint.service(frame, offset)
            calls += 1
        self.assertGreater(calls, 1)
        self.assertEqual(bytes(endpoint.output), frame)

    def test_zero_write_after_reported_capacity_does_not_advance(self):
        endpoint = FakeEndpoint([8, 8], zero_writes=1)
        self.assertEqual(endpoint.service(b"abcdef", 0), 0)
        self.assertEqual(endpoint.service(b"abcdef", 0), 6)

    def test_per_call_budget(self):
        frame = bytes(100)
        endpoint = FakeEndpoint([100])
        self.assertEqual(endpoint.service(frame, 0), 32)


class QueueTests(unittest.TestCase):
    def test_sequence_continuity_and_retention(self):
        queue = ReliableQueueModel()
        self.assertTrue(queue.enqueue("A"))
        self.assertTrue(queue.enqueue("B"))
        self.assertEqual([r[0] for r in queue.records], [1, 2])
        queue.transmit_next()
        self.assertEqual(len(queue.records), 2)

    def test_cumulative_duplicate_and_partial_ack(self):
        queue = ReliableQueueModel()
        for name in "ABC":
            queue.enqueue(name)
            queue.transmit_next()
        self.assertTrue(queue.acknowledge(77, 2))
        self.assertEqual([r[0] for r in queue.records], [3])
        self.assertTrue(queue.acknowledge(77, 2))

    def test_wrong_boot_and_ack_beyond_emitted(self):
        queue = ReliableQueueModel()
        queue.enqueue("A")
        self.assertFalse(queue.acknowledge(99, 0))
        self.assertFalse(queue.acknowledge(77, 1))

    def test_resend_results(self):
        queue = ReliableQueueModel()
        for name in "ABC":
            queue.enqueue(name)
            queue.transmit_next()
        self.assertEqual(queue.resend(77, 2), [2, 3])
        self.assertEqual(queue.resend(99, 2), "wrong_boot")
        queue.acknowledge(77, 2)
        self.assertEqual(queue.resend(77, 1), "too_old")

    def test_queue_full_latches_only_first_loss_without_gap(self):
        queue = ReliableQueueModel(capacity=2)
        queue.enqueue("A")
        queue.enqueue("B")
        self.assertFalse(queue.enqueue("FIRST_LOST", 10))
        self.assertFalse(queue.enqueue("SECOND_LOST", 11))
        self.assertEqual(queue.overflow, ("FIRST_LOST", 10))
        self.assertEqual(queue.next_sequence, 3)

    def test_sequence_continues_after_partial_ack_and_new_run_activity(self):
        queue = ReliableQueueModel()
        queue.enqueue("RUN_ONE")
        queue.transmit_next()
        queue.acknowledge(77, 1)
        queue.enqueue("RUN_TWO")
        self.assertEqual(queue.records[0][0], 2)

    def test_new_boot_has_independent_identity_and_sequence(self):
        first = ReliableQueueModel(boot=1)
        second = ReliableQueueModel(boot=2)
        first.enqueue("A")
        second.enqueue("A")
        self.assertNotEqual(first.boot, second.boot)
        self.assertEqual(first.records[0][0], second.records[0][0])


class FirmwarePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.transport = (ROOT / "src/SerialTransport.cpp").read_text()
        cls.controller = (ROOT / "src/MouseHouse.cpp").read_text()

    def test_only_transport_writes_usb(self):
        self.assertNotIn("Serial.print", self.controller)
        self.assertNotIn("Serial.println", self.controller)
        self.assertIn("Serial.write(encoded_ + encodedOffset_, requested)", self.transport)

    def test_no_blocking_stream_apis(self):
        combined = self.transport + self.controller
        for forbidden in ("Serial.flush", "readString", "readStringUntil", "parseInt"):
            self.assertNotIn(forbidden, combined)

    def test_v2_has_no_per_frame_edge_loop(self):
        camera = self.controller.split("void MouseHouse::drainCameraEvents()", 1)[1]
        v2_branch = camera.split("if (transport_.v2EnabledOrPending())", 1)[1]
        v2_branch = v2_branch.split("uint32_t emitted", 1)[0]
        self.assertNotIn("CAMERA_HIGH", v2_branch)
        self.assertNotIn("CAMERA_LOW", v2_branch)

    def test_v1_camera_edges_remain(self):
        self.assertIn('logEvent("CAMERA_HIGH"', self.controller)
        self.assertIn('logEvent("CAMERA_LOW"', self.controller)

    def test_negotiation_ack_is_exact_and_deferred(self):
        self.assertIn('const char response[] = "ACK_PROTO,2\\n";', self.transport)
        self.assertIn("if (activate) mode_ = PROTOCOL_V2_ACTIVE;", self.transport)

    def test_python_wire_constants_match_shared_header(self):
        header = (ROOT / "src/ProtocolV2.h").read_text()
        self.assertRegex(header, r"kVersion = 2;")
        self.assertRegex(header, rf"kMaxPayloadSize = {MAX_PAYLOAD};")
        self.assertEqual(HEADER.size, 34)

    def test_checkpoint_pressure_does_not_allocate_a_record(self):
        camera = self.controller.split("void MouseHouse::drainCameraEvents()", 1)[1]
        pressure = camera.split("if (d.queueUsed <", 1)[1].split("cameraLoggedFrameCount_", 1)[0]
        self.assertIn("noteSuppressedCheckpoint", pressure)
        suppressed_branch = pressure.split("else", 1)[1]
        self.assertNotIn("enqueueCamera", suppressed_branch)


if __name__ == "__main__":
    unittest.main()
