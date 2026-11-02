"""Idle-only clock transport for a fresh boot or an already-v2 controller."""
from __future__ import annotations

from pathlib import Path
import secrets
import time
import uuid

from . import controller_protocol_v2 as protocol


def journal_path_for(output: Path) -> Path:
    return output.with_name(f"{output.name}.transport-{uuid.uuid4().hex}.jsonl")


class ClockTransport:
    """Detect outbound framing and fsync reliable replies before cumulative ACK.

    A fresh, echoed TIME_SYNC challenge proves that the controller is idle and
    establishes this utility invocation's sequence baseline. Startup traffic is
    discarded before that boundary, never presented as recovered experiment
    data. No PROTO, START, STOP, or reset command is sent.
    """

    def __init__(self, port, journal_path: Path):
        self.port = port
        self.journal_path = Path(journal_path)
        self._journal = None
        self._buffer = bytearray()
        self._v2 = False
        self._boot = None
        self._baseline = None
        self._proof = None
        self.startup_discarded_records = 0

    def __enter__(self):
        if self.journal_path.exists():
            raise ValueError("clock transport journal must be a new file")
        request_id, sent_ns = secrets.randbits(32), time.time_ns()
        self._proof = f"CLOCK_SYNC,{request_id},{sent_ns},"
        self.write(f"TIME_SYNC,{request_id},{sent_ns}\n".encode())
        try:
            self.wait_for("CLOCK_SYNC,", match=lambda text: text.startswith(self._proof))
        except BaseException:
            self.close()
            raise
        self._proof = None
        return self

    def __exit__(self, *_args):
        self.close()

    def close(self):
        journal, self._journal = self._journal, None
        if journal is not None:
            journal.close()

    def write(self, command: bytes):
        count = self.port.write(command)
        if count != len(command):
            raise OSError("short clock command write")
        return count

    def _record(self, deadline):
        while time.monotonic() < deadline:
            value = self.port.read(1)
            if not value:
                continue
            self._buffer.extend(value)
            binary = self._v2 or (len(self._buffer) >= 2 and self._buffer[1] == protocol.VERSION)
            if value == b"\0":
                encoded = bytes(self._buffer[:-1])
                self._buffer.clear()
                try:
                    frame = protocol.decode_frame(encoded)
                except protocol.ProtocolV2Error:
                    if self._baseline is not None:
                        raise
                    self.startup_discarded_records += 1
                    continue  # Reconnect can begin in the tail of an old frame.
                self._v2 = True
                return frame.text, frame, time.time_ns()
            if value == b"\n" and not binary:
                raw = bytes(self._buffer)
                self._buffer.clear()
                try:
                    return raw.decode("utf-8", errors="strict").strip(), None, time.time_ns()
                except UnicodeDecodeError:
                    if self._baseline is not None:
                        raise
                    self.startup_discarded_records += 1
            limit = protocol.MAX_ENCODED_SIZE if binary else 4096
            if len(self._buffer) > limit:
                self._buffer.clear()
                raise ValueError("clock response exceeds bounded frame size")
        raise TimeoutError("No matching clock response")

    def wait_for(self, prefix: str, timeout: float = 3.0, *, match=None):
        deadline = time.monotonic() + timeout
        pending = None
        while time.monotonic() < deadline:
            text, frame, received_ns = self._record(deadline)
            matches = text.startswith(prefix) and (match is None or match(text))
            if frame is None:
                if text.startswith("NACK,"):
                    raise RuntimeError(text)
                if matches:
                    return text, received_ns
                self.startup_discarded_records += 1
                continue
            if frame.integrity_latched or frame.message_type == protocol.MessageType.INTEGRITY_FAULT:
                raise RuntimeError("controller integrity fault: " + text)
            if self._baseline is None:
                if text.startswith("NACK,TIME_SYNC,"):
                    raise RuntimeError(text)
                if not (matches and self._proof and text.startswith(self._proof)
                        and frame.session_id == 0 and frame.reliable and frame.sequence > 0
                        and frame.message_type == protocol.MessageType.COMMAND_RESULT):
                    self.startup_discarded_records += 1
                    continue
                fields = text.split(",")
                if len(fields) != 7 or not all(value.isdecimal() for value in fields[1:6]):
                    raise ValueError("malformed clock startup proof")
                self._boot, self._baseline = frame.boot_id, frame.sequence
                self._journal = protocol.DurableJournal(self.journal_path, allow_midstream_start=True)
            if frame.boot_id != self._boot:
                raise RuntimeError("controller reboot during clock validation")
            if frame.session_id != 0:
                raise RuntimeError("clock utility requires an idle controller")
            if not frame.reliable or not frame.sequence:
                raise ValueError("clock response must be reliable")
            if frame.sequence < self._baseline:
                continue  # Late traffic from before this invocation's proof.
            result, contiguous = self._journal.store(
                frame, host_unix_ns=received_ns, host_monotonic_ns=time.monotonic_ns())
            if result == protocol.StoreResult.CONFLICT:
                raise RuntimeError("conflicting clock response replay")
            if contiguous:
                self.write(f"ACK_EVENTS,{self._boot},{contiguous}\n".encode())
            if frame.sequence > contiguous + 1:
                self.write(f"RESEND_EVENTS,{self._boot},{contiguous + 1}\n".encode())
            if result == protocol.StoreResult.NEW:
                if text.startswith("NACK,"):
                    raise RuntimeError(text)
                if matches:
                    pending = (text, received_ns, frame.sequence)
            if pending is not None and contiguous >= pending[2]:
                return pending[:2]
        raise TimeoutError(f"No {prefix} response")
