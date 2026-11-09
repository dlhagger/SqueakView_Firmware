"""Clock retries must be independent of callbacks from retired attempts."""
import unittest
from unittest import mock

from PySide6.QtWidgets import QApplication
from squeakview_firmware import qualification_gui as gui


class QualificationClockGuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        with mock.patch.object(gui.list_ports, 'comports', return_value=[]):
            self.window = gui.QualificationWindow()
        self.window.port_timer.stop()
        self.window.camera_display_timer.stop()
        self.window.port = mock.Mock()
        self.window.state.stage = 'RTC'
        self.window.state.protocol_version = gui.PROTOCOL_VERSION
        self.addCleanup(self.window.close)
        status = mock.patch.object(gui, 'host_time_status', return_value={'ntp_synchronized': True})
        status.start()
        self.addCleanup(status.stop)
        timer = mock.patch.object(gui.QTimer, 'singleShot')
        self.timers = timer.start()
        self.addCleanup(timer.stop)

    def deadline(self):
        return next(call.args[1] for call in self.timers.call_args_list if call.args[0] == 3000)

    def test_missing_correction_ack_expires_and_allows_retry(self):
        self.window.clock_gate_state = 'correction_required'
        self.window.clock_gate_action()
        self.assertFalse(self.window.rtc_button.isEnabled())
        self.deadline()()
        self.assertEqual(self.window.clock_gate_state, 'failed')
        self.assertEqual(self.window.clock_validation_record['reason'], 'RTC_CORRECTION_TIMEOUT')
        self.assertTrue(self.window.rtc_button.isEnabled())

    def test_ack_retires_correction_deadline_before_verification_starts(self):
        self.window.clock_gate_state = 'correction_required'
        self.window.clock_gate_action()
        old_deadline = self.deadline()
        self.window._handle_line('ACK_SET_RTC,1700000000,100,1')
        old_deadline()
        self.assertEqual(self.window.clock_gate_state, 'correcting')
        verify = next(call.args[1] for call in self.timers.call_args_list if call.args[0] == 100)
        verify()
        self.assertEqual(self.window.clock_gate_state, 'validating_after')

    def test_old_sample_timeout_cannot_fail_retry_with_same_sequence(self):
        self.window._start_clock_validation(False)
        old_timeout = self.deadline()
        self.window._fail_clock_gate('TEST_FAILURE', 'first attempt failed')
        self.window._start_clock_validation(False)
        old_timeout()
        self.assertEqual(self.window.clock_gate_state, 'validating_before')
        self.assertEqual(self.window.clock_gate_pending_sequence, 0)

    def test_current_sample_timeout_still_fails(self):
        self.window._start_clock_validation(False)
        self.deadline()()
        self.assertEqual(self.window.clock_validation_record['reason'], 'CLOCK_RESPONSE_TIMEOUT')

    def test_retired_response_does_not_fail_new_sample(self):
        self.window._start_clock_validation(False)
        sent = self.window.clock_gate_pending_sent_ns
        self.window._fail_clock_gate('TEST_FAILURE', 'retry')
        self.window._start_clock_validation(False)
        self.window._handle_clock_sync_response(f'CLOCK_SYNC,0,{sent},1000,1100,1000000,RTC_VALID', sent + 1000000)
        self.assertEqual(self.window.clock_gate_state, 'validating_before')
        self.assertEqual(self.window.clock_gate_samples, [])

    def test_disconnect_retires_scheduled_sample_send(self):
        self.window._start_clock_validation(False)
        sent = self.window.clock_gate_pending_sent_ns
        self.window._handle_clock_sync_response(f'CLOCK_SYNC,0,{sent},1000,1100,1000000,RTC_VALID', sent + 1000000)
        send_next = next(call.args[1] for call in self.timers.call_args_list if call.args[0] == 100)
        self.window.disconnect_serial()
        new_port = mock.Mock()
        self.window.port = new_port
        send_next()
        new_port.write.assert_not_called()
