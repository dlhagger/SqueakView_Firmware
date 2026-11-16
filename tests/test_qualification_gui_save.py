"""Qualification evidence must remain retryable until both files are saved."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from PySide6.QtWidgets import QApplication
from squeakview_firmware import qualification_gui as gui


class QualificationSaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        with mock.patch.object(gui.list_ports, "comports", return_value=[]):
            self.window = gui.QualificationWindow()
        self.window.port_timer.stop()
        self.window.camera_display_timer.stop()
        self.addCleanup(self.window.close)
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.output = Path(directory.name)
        self.window.output_directory = self.output
        self.window.output_edit.setText(str(self.output))
        self.line = "SETUP_QUALIFICATION_RECORD,device=TEST,result=PASS,timestamp_us=1"
        popup = mock.patch.object(gui.QMessageBox, "critical")
        self.error = popup.start()
        self.addCleanup(popup.stop)

    def test_each_write_failure_is_reported_and_retry_saves_both_files(self):
        for extension in (".txt", ".json"):
            with self.subTest(extension=extension):
                self.window.saved_record_line = ""
                write = self.window._write_record_file

                def failing_write(path, content):
                    if path.suffix == extension:
                        raise OSError("test disk full")
                    write(path, content)

                with mock.patch.object(self.window, "_write_record_file", side_effect=failing_write):
                    self.window._handle_line(self.line)
                self.assertEqual(self.window.saved_record_line, "")
                self.assertEqual(self.window.pending_record_line, self.line)
                self.assertIn("NOT saved", self.window.notice.text())
                self.assertFalse(self.window.save_record_button.isHidden())
                self.window.retry_save_record()
                self.assertEqual(self.window.saved_record_line, self.line)
                self.assertEqual(self.window.pending_record_line, "")
                self.assertIn("saved locally", self.window.notice.text())
                self.assertTrue(self.window.save_record_button.isHidden())
                records = list(self.output.glob("*.json"))
                self.assertTrue(records)
                for path in records:
                    self.assertEqual(json.loads(path.read_text())["raw_record"], self.line)
                    self.assertEqual(path.with_suffix(".txt").read_text(), self.line + "\n")

    def test_retry_can_change_directory_and_duplicate_success_does_not_rewrite(self):
        blocked = self.output / "file"
        blocked.write_text("not a directory")
        self.window.output_directory = blocked
        self.window._handle_line(self.line)
        self.window.retry_save_record()
        self.assertEqual(self.window.pending_record_line, "")
        files = sorted(self.output.glob("*_qualification.*"))
        self.assertEqual(len(files), 2)
        with mock.patch.object(self.window, "_write_record_file") as write:
            self.window._handle_line(self.line)
            write.assert_not_called()

    def test_atomic_replace_failure_leaves_no_partial_or_temp_file(self):
        path = self.output / "record.json"
        with mock.patch.object(gui.os, "replace", side_effect=OSError("replace failed")):
            with self.assertRaises(OSError):
                self.window._write_record_file(path, '{"result":"PASS"}')
        self.assertEqual(list(self.output.iterdir()), [])

    def test_new_connection_cannot_discard_unsaved_record(self):
        self.window.pending_record_line = self.line
        with mock.patch.object(self.window, "retry_save_record") as retry, \
                mock.patch.object(gui.serial, "Serial") as serial:
            self.window.toggle_connection()
            retry.assert_called_once()
            serial.assert_not_called()
