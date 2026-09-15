"""PySide6 operator interface for MouseHouse pre-deployment qualification."""

from __future__ import annotations

import json
import re
import sys
import time
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports
from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSplitter,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .qualification import CHECK_LABELS, PROTOCOL_VERSION, QualificationState, parse_fields


APP_STYLE = """
QMainWindow { background: #10151d; }
QWidget { color: #e8eef7; font-size: 14px; }
QFrame#card { background: #18212d; border: 1px solid #2a394b; border-radius: 12px; }
QLabel#eyebrow { color: #75b9ff; font-weight: 700; letter-spacing: 1px; }
QLabel#title { font-size: 28px; font-weight: 750; color: #ffffff; }
QLabel#instruction { font-size: 17px; color: #c8d4e3; }
QLabel#connectionGood { color: #62d995; font-weight: 700; }
QLabel#connectionBad { color: #ff7887; font-weight: 700; }
QLabel#notice { color: #ffd479; font-weight: 650; }
QLineEdit, QComboBox, QPlainTextEdit, QTreeWidget {
  background: #0d131b; border: 1px solid #34465a; border-radius: 7px;
  color: #e8eef7; padding: 7px; selection-background-color: #2476c8;
}
QPushButton { background: #27384b; border: 1px solid #3b536d; border-radius: 8px;
  padding: 9px 15px; font-weight: 650; }
QPushButton:hover { background: #324a63; }
QPushButton:disabled { color: #697889; background: #1a232e; }
QPushButton#primary { background: #1675d1; border-color: #3c9aff; color: white; }
QPushButton#success { background: #197548; border-color: #32b875; color: white; }
QPushButton#danger { background: #7e2935; border-color: #c74b5c; color: white; }
QTreeWidget { outline: none; }
QHeaderView::section { background: #18212d; color: #9eb0c4; border: 0; padding: 7px; }
QScrollBar:vertical { background: #10151d; width: 12px; }
QScrollBar::handle:vertical { background: #34465a; border-radius: 5px; min-height: 24px; }
"""


class QualificationWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("MouseHouse Hardware Qualification")
        self.resize(1180, 760)
        self.setMinimumSize(920, 620)
        self.state = QualificationState()
        self.port: serial.Serial | None = None
        self.rx_buffer = bytearray()
        self.transcript: list[str] = []
        self.transcript_path: Path | None = None
        self.saved_record_line = ""
        self.output_directory = Path.home() / "MouseHouseQualificationLogs"
        self.check_items: dict[str, QTreeWidgetItem] = {}
        self._build_ui()
        self.setStyleSheet(APP_STYLE)

        self.read_timer = QTimer(self)
        self.read_timer.setInterval(25)
        self.read_timer.timeout.connect(self._read_serial)
        self.port_timer = QTimer(self)
        self.port_timer.setInterval(2000)
        self.port_timer.timeout.connect(self._refresh_ports_silently)
        self.port_timer.start()
        self.refresh_ports()
        self._render()

    def _card(self) -> tuple[QFrame, QVBoxLayout]:
        frame = QFrame()
        frame.setObjectName("card")
        layout = QVBoxLayout(frame)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(10)
        return frame, layout

    def _build_ui(self) -> None:
        root = QWidget()
        outer = QVBoxLayout(root)
        outer.setContentsMargins(18, 18, 18, 18)
        outer.setSpacing(14)
        self.setCentralWidget(root)

        heading = QHBoxLayout()
        title_box = QVBoxLayout()
        title = QLabel("MouseHouse Qualification")
        title.setObjectName("title")
        subtitle = QLabel("Guided pre-deployment hardware and firmware validation")
        subtitle.setStyleSheet("color:#91a4ba")
        title_box.addWidget(title)
        title_box.addWidget(subtitle)
        heading.addLayout(title_box)
        heading.addStretch()
        self.connection_badge = QLabel("● Disconnected")
        self.connection_badge.setObjectName("connectionBad")
        heading.addWidget(self.connection_badge)
        outer.addLayout(heading)

        connection, connection_layout = self._card()
        row = QHBoxLayout()
        row.addWidget(QLabel("Serial device"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(280)
        row.addWidget(self.port_combo)
        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.clicked.connect(self.refresh_ports)
        row.addWidget(self.refresh_button)
        self.connect_button = QPushButton("Connect")
        self.connect_button.setObjectName("primary")
        self.connect_button.clicked.connect(self.toggle_connection)
        row.addWidget(self.connect_button)
        row.addSpacing(16)
        row.addWidget(QLabel("Save records to"))
        self.output_edit = QLineEdit(str(self.output_directory))
        self.output_edit.setCursorPosition(0)
        row.addWidget(self.output_edit, 1)
        browse = QPushButton("Browse")
        browse.clicked.connect(self.choose_output_directory)
        row.addWidget(browse)
        connection_layout.addLayout(row)
        outer.addWidget(connection)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.setChildrenCollapsible(False)
        outer.addWidget(splitter, 1)

        progress_card, progress_layout = self._card()
        progress_layout.addWidget(self._eyebrow("QUALIFICATION PROGRESS"))
        self.progress_summary = QLabel("0 of 19 checks complete")
        self.progress_summary.setStyleSheet("font-size:18px;font-weight:700")
        progress_layout.addWidget(self.progress_summary)
        self.check_tree = QTreeWidget()
        self.check_tree.setHeaderLabels(["Check", "Status"])
        self.check_tree.setColumnWidth(0, 230)
        self.check_tree.setRootIsDecorated(False)
        self.check_tree.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        for key, label in CHECK_LABELS.items():
            item = QTreeWidgetItem([label, "Waiting"])
            self.check_tree.addTopLevelItem(item)
            self.check_items[key] = item
        progress_layout.addWidget(self.check_tree, 1)
        splitter.addWidget(progress_card)

        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(14)

        action_card, action_layout = self._card()
        self.stage_label = self._eyebrow("CONNECT A DEVICE")
        action_layout.addWidget(self.stage_label)
        self.action_title = QLabel("Ready to begin")
        self.action_title.setObjectName("title")
        self.action_title.setWordWrap(True)
        action_layout.addWidget(self.action_title)
        self.instructions = QLabel(
            "Connect the Feather RP2040 running the setup_debug firmware."
        )
        self.instructions.setObjectName("instruction")
        self.instructions.setWordWrap(True)
        self.instructions.setMinimumHeight(70)
        action_layout.addWidget(self.instructions)
        self.notice = QLabel("")
        self.notice.setObjectName("notice")
        self.notice.setWordWrap(True)
        action_layout.addWidget(self.notice)

        self.device_row = QWidget()
        device_layout = QHBoxLayout(self.device_row)
        device_layout.setContentsMargins(0, 0, 0, 0)
        self.device_edit = QLineEdit()
        self.device_edit.setPlaceholderText("e.g. MH-012")
        self.device_edit.returnPressed.connect(self.submit_device_id)
        device_layout.addWidget(self.device_edit, 1)
        self.device_button = QPushButton("Begin qualification")
        self.device_button.setObjectName("primary")
        self.device_button.clicked.connect(self.submit_device_id)
        device_layout.addWidget(self.device_button)
        action_layout.addWidget(self.device_row)

        self.special_row = QWidget()
        special_layout = QHBoxLayout(self.special_row)
        special_layout.setContentsMargins(0, 0, 0, 0)
        self.rtc_button = QPushButton("Set RTC from this computer")
        self.rtc_button.clicked.connect(self.set_rtc)
        self.clear_jam_button = QPushButton("I cleared the mechanism — Clear Jam")
        self.clear_jam_button.clicked.connect(lambda: self.send_command("CLEAR_JAM"))
        self.camera_start_button = QPushButton("Start 30 FPS")
        self.camera_start_button.clicked.connect(lambda: self.send_command("START,30"))
        self.camera_stop_button = QPushButton("Stop camera test")
        self.camera_stop_button.clicked.connect(lambda: self.send_command("STOP"))
        special_layout.addWidget(self.rtc_button)
        special_layout.addWidget(self.clear_jam_button)
        special_layout.addWidget(self.camera_start_button)
        special_layout.addWidget(self.camera_stop_button)
        action_layout.addWidget(self.special_row)

        confirmation = QHBoxLayout()
        self.yes_button = QPushButton("Yes — Pass")
        self.yes_button.setObjectName("success")
        self.yes_button.clicked.connect(lambda: self.send_command("TEST,YES"))
        self.no_button = QPushButton("No — Fail and repeat")
        self.no_button.setObjectName("danger")
        self.no_button.clicked.connect(lambda: self.send_command("TEST,NO"))
        self.retry_button = QPushButton("Replay / Retry")
        self.retry_button.clicked.connect(lambda: self.send_command("TEST,RETRY"))
        confirmation.addWidget(self.yes_button)
        confirmation.addWidget(self.no_button)
        confirmation.addWidget(self.retry_button)
        action_layout.addLayout(confirmation)
        right_layout.addWidget(action_card)

        utility = QHBoxLayout()
        self.status_button = QPushButton("Refresh status")
        self.status_button.clicked.connect(lambda: self.send_command("TEST,STATUS"))
        self.restart_button = QPushButton("Restart qualification")
        self.restart_button.clicked.connect(self.restart_qualification)
        self.abort_button = QPushButton("Abort")
        self.abort_button.clicked.connect(lambda: self.send_command("TEST,ABORT"))
        utility.addWidget(self.status_button)
        utility.addStretch()
        utility.addWidget(self.restart_button)
        utility.addWidget(self.abort_button)
        right_layout.addLayout(utility)

        log_card, log_layout = self._card()
        log_header = QHBoxLayout()
        log_header.addWidget(self._eyebrow("DEVICE LOG"))
        log_header.addStretch()
        self.log_path_label = QLabel("Not recording")
        self.log_path_label.setStyleSheet("color:#91a4ba")
        log_header.addWidget(self.log_path_label)
        log_layout.addLayout(log_header)
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(3000)
        mono = QFont("Monospace")
        mono.setStyleHint(QFont.StyleHint.Monospace)
        self.log_view.setFont(mono)
        log_layout.addWidget(self.log_view, 1)
        right_layout.addWidget(log_card, 1)
        splitter.addWidget(right)
        splitter.setSizes([360, 760])

    @staticmethod
    def _eyebrow(text: str) -> QLabel:
        label = QLabel(text)
        label.setObjectName("eyebrow")
        return label

    def choose_output_directory(self) -> None:
        chosen = QFileDialog.getExistingDirectory(
            self, "Choose qualification log directory", self.output_edit.text()
        )
        if chosen:
            self.output_edit.setText(chosen)

    def refresh_ports(self) -> None:
        current = self.port_combo.currentData() or self.port_combo.currentText()
        ports = sorted(list_ports.comports(), key=lambda item: item.device)
        self.port_combo.clear()
        for info in ports:
            label = f"{info.device} — {info.description}"
            self.port_combo.addItem(label, info.device)
        if current:
            for index in range(self.port_combo.count()):
                if self.port_combo.itemData(index) == current:
                    self.port_combo.setCurrentIndex(index)
                    break
        if ports and self.port_combo.currentIndex() < 0:
            self.port_combo.setCurrentIndex(0)

    def _refresh_ports_silently(self) -> None:
        if self.port is None:
            self.refresh_ports()

    def toggle_connection(self) -> None:
        if self.port is not None:
            self.disconnect_serial()
            return
        device = self.port_combo.currentData()
        if not device:
            QMessageBox.warning(self, "No serial device", "Connect the board and refresh the device list.")
            return
        try:
            self.port = serial.Serial(
                device, 115200, timeout=0, write_timeout=0.5, exclusive=True
            )
        except (serial.SerialException, OSError, ValueError) as exc:
            QMessageBox.critical(self, "Connection failed", str(exc))
            self.port = None
            return
        self.state.reset()
        self.rx_buffer.clear()
        self.transcript.clear()
        self.saved_record_line = ""
        self.output_directory = Path(self.output_edit.text()).expanduser()
        try:
            self.output_directory.mkdir(parents=True, exist_ok=True)
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            self.transcript_path = self.output_directory / f"qualification_{stamp}.log"
        except OSError as exc:
            QMessageBox.warning(self, "Cannot create log", str(exc))
            self.transcript_path = None
        self.connection_badge.setText(f"● Connected: {device}")
        self.connection_badge.setObjectName("connectionGood")
        self.connection_badge.style().unpolish(self.connection_badge)
        self.connection_badge.style().polish(self.connection_badge)
        self.connect_button.setText("Disconnect")
        self.port_combo.setEnabled(False)
        self.read_timer.start()
        self._append_log(f"Connected to {device}", "HOST")
        QTimer.singleShot(750, lambda: self.send_command("TEST,HELLO"))
        self._render()

    def disconnect_serial(self, reason: str = "Disconnected") -> None:
        self.read_timer.stop()
        if self.port is not None:
            try:
                self.port.close()
            except serial.SerialException:
                pass
        self.port = None
        self.connection_badge.setText(f"● {reason}")
        self.connection_badge.setObjectName("connectionBad")
        self.connection_badge.style().unpolish(self.connection_badge)
        self.connection_badge.style().polish(self.connection_badge)
        self.connect_button.setText("Connect")
        self.port_combo.setEnabled(True)
        self._render()

    def _read_serial(self) -> None:
        if self.port is None:
            return
        try:
            waiting = self.port.in_waiting
            if waiting:
                self.rx_buffer.extend(self.port.read(waiting))
        except (serial.SerialException, OSError) as exc:
            self._append_log(str(exc), "ERROR")
            self.disconnect_serial("Connection lost")
            return
        while b"\n" in self.rx_buffer:
            raw, _, remainder = self.rx_buffer.partition(b"\n")
            self.rx_buffer = bytearray(remainder)
            line = raw.decode(errors="replace").strip("\r ")
            if line:
                self._handle_line(line)

    def _handle_line(self, line: str) -> None:
        self._append_log(line, "RX")
        event = self.state.apply_line(line)
        if line.startswith("SETUP_LED_TEST,"):
            color = line.split(",", 1)[1]
            self.notice.setText(f"LED sweep currently showing: {color}")
        elif event == "step_result":
            _, _, values = parse_fields(line)
            result = values.get("result", "")
            step = values.get("step", "test").replace("_", " ").title()
            self.notice.setText(f"{step}: {result}")
        elif event == "error":
            if not self.state.protocol_version:
                self.notice.setText(
                    "This is not the matching setup_debug firmware. Flash the current build and reconnect."
                )
            else:
                self.notice.setText(f"Controller rejected a command: {line}")
        elif event == "protocol":
            if self.state.protocol_version == PROTOCOL_VERSION:
                self.notice.setText("Firmware protocol verified. Follow the current instruction.")
            else:
                self.notice.setText(
                    "Firmware/GUI protocol mismatch. Flash the current setup_debug firmware."
                )
        elif event == "record":
            self._save_final_record(line)
            result = (self.state.final_record or {}).get("result", "UNKNOWN")
            self.notice.setText(f"Qualification {result}. Record saved locally.")
        self._render()

    def send_command(self, command: str) -> None:
        if self.port is None:
            QMessageBox.warning(self, "Not connected", "Connect to the MouseHouse controller first.")
            return
        try:
            self.port.write(f"{command}\n".encode())
            self.port.flush()
            self._append_log(command, "TX")
        except (serial.SerialException, OSError) as exc:
            self._append_log(str(exc), "ERROR")
            self.disconnect_serial("Connection lost")

    def submit_device_id(self) -> None:
        device_id = self.device_edit.text().strip()
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,31}", device_id):
            QMessageBox.warning(
                self,
                "Invalid device ID",
                "Use 1–31 letters, numbers, underscores, or hyphens.",
            )
            return
        self.send_command(f"TEST,DEVICE,{device_id}")

    def set_rtc(self) -> None:
        self.send_command(f"SET_RTC,{int(time.time())}")

    def restart_qualification(self) -> None:
        answer = QMessageBox.question(
            self,
            "Restart qualification?",
            "This clears the current in-memory checklist and begins again.",
        )
        if answer == QMessageBox.StandardButton.Yes:
            self.send_command("TEST,RESTART")

    def _append_log(self, line: str, direction: str) -> None:
        timestamp = datetime.now().isoformat(timespec="milliseconds")
        rendered = f"{timestamp} [{direction}] {line}"
        self.transcript.append(rendered)
        self.log_view.appendPlainText(rendered)
        if self.transcript_path is not None:
            try:
                with self.transcript_path.open("a", encoding="utf-8") as handle:
                    handle.write(rendered + "\n")
                self.log_path_label.setText(str(self.transcript_path))
            except OSError as exc:
                self.log_path_label.setText(f"Log write failed: {exc}")

    def _save_final_record(self, line: str) -> None:
        if line == self.saved_record_line:
            return
        self.saved_record_line = line
        record = self.state.final_record or {}
        safe_id = re.sub(r"[^A-Za-z0-9_-]", "_", record.get("device", "unknown"))
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        try:
            self.output_directory.mkdir(parents=True, exist_ok=True)
            base = self.output_directory / f"{safe_id}_{stamp}_qualification"
            base.with_suffix(".txt").write_text(line + "\n", encoding="utf-8")
            base.with_suffix(".json").write_text(
                json.dumps(
                    {"record": record, "raw_record": line, "transcript": self.transcript},
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )
            self.log_path_label.setText(f"Saved: {base}.json")
        except OSError as exc:
            QMessageBox.critical(self, "Could not save qualification record", str(exc))

    def _render(self) -> None:
        connected = self.port is not None
        protocol_ok = connected and self.state.protocol_version == PROTOCOL_VERSION
        stage = self.state.stage
        self.stage_label.setText(f"CURRENT STAGE  •  {stage.replace('_', ' ')}")
        self.action_title.setText(self.state.title if connected else "Connect the controller")
        self.instructions.setText(
            self.state.instructions
            if connected
            else "Close any PlatformIO serial monitor, select the RP2040 serial device, and connect."
        )
        complete = sum(self.state.checks.values())
        self.progress_summary.setText(f"{complete} of {len(self.state.checks)} checks complete")
        for key, item in self.check_items.items():
            passed = self.state.checks[key]
            item.setText(1, "Passed" if passed else "Waiting")
            item.setForeground(1, QColor("#62d995" if passed else "#8292a5"))

        self.device_row.setVisible(protocol_ok and stage == "DEVICE_ID")
        confirming = protocol_ok and self.state.awaiting_confirmation
        self.yes_button.setVisible(confirming)
        self.no_button.setVisible(confirming)
        self.retry_button.setVisible(confirming)
        self.rtc_button.setVisible(protocol_ok and stage == "RTC")
        self.clear_jam_button.setVisible(protocol_ok and stage == "JAM_CLEAR")
        camera = protocol_ok and stage == "CAMERA_CYCLE"
        self.camera_start_button.setVisible(camera)
        self.camera_stop_button.setVisible(camera)
        self.status_button.setEnabled(protocol_ok)
        self.restart_button.setEnabled(protocol_ok)
        self.abort_button.setEnabled(protocol_ok and stage not in {"COMPLETE", "ABORTED"})

    def closeEvent(self, event) -> None:  # type: ignore[no-untyped-def]
        self.disconnect_serial()
        event.accept()


def main() -> None:
    app = QApplication(sys.argv)
    app.setApplicationName("MouseHouse Qualification")
    window = QualificationWindow()
    window.show()
    raise SystemExit(app.exec())


if __name__ == "__main__":
    main()
