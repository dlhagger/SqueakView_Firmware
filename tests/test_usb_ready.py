import unittest
import tempfile
from pathlib import Path
from unittest.mock import Mock, patch


from squeakview_firmware import usb_recovery as usb


class UsbReadyTests(unittest.TestCase):
    def test_no_arguments_accepts_detected_board_without_reset(self):
        with patch.object(usb.Controller, 'ready', return_value=True), \
                patch.object(usb.os, 'open') as open_lock:
            self.assertEqual(usb.main([]), 0)
            open_lock.assert_not_called()

    def test_automatic_discovery_missing_present_and_ambiguous(self):
        with tempfile.TemporaryDirectory() as directory:
            controller = usb.Controller()
            controller.device = Path(directory) / controller.device.name
            self.assertFalse(controller.ready())
            first = Path(directory) / 'usb-Adafruit_Feather_RP2040_Adalogger_FIRST-if00'
            first.symlink_to('/dev/null')
            self.assertTrue(controller.ready())
            self.assertEqual(controller.device, first)
            other = Path(directory) / 'usb-Adafruit_Feather_RP2040_Adalogger_SECOND-if00'
            other.symlink_to('/dev/null')
            controller = usb.Controller()
            controller.device = Path(directory) / controller.device.name
            with self.assertRaisesRegex(RuntimeError, 'Multiple controllers'):
                controller.ready()

    def test_automatic_recovery_accepts_matching_usb_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            port = Path(directory)
            for name, value in [('idVendor', '239a'), ('idProduct', '815d'),
                                ('serial', 'ANY_BOARD')]:
                (port / name).write_text(value)
            controller = usb.Controller()
            with patch.object(controller, 'ready', return_value=False), \
                    patch.object(usb, 'USB_PORT', port), \
                    patch.object(usb, 'ROLE') as role, \
                    patch.object(usb.time, 'sleep'):
                role.read_text.return_value = 'host'
                controller.reset_role()
                self.assertEqual([call.args[0] for call in role.write_text.call_args_list],
                                 ['none\n', 'host\n'])

    def test_missing_device_requires_explicit_privilege(self):
        with patch.object(usb.Controller, 'ready', return_value=False), \
                patch.object(usb.os, 'geteuid', return_value=1000), \
                patch.object(usb.Controller, 'reset_role') as reset:
            self.assertEqual(usb.main(['--serial', 'TEST']), 1)
            reset.assert_not_called()

    def test_present_device_needs_no_privilege_or_lock(self):
        with patch.object(usb.Controller, 'ready', return_value=True), \
                patch.object(usb.os, 'open') as open_lock:
            self.assertEqual(usb.main(['--serial', 'TEST']), 0)
            open_lock.assert_not_called()

    def controller(self):
        controller = Mock()
        controller.ready.return_value = False
        controller.wait.return_value = False
        return controller

    def test_present_device_is_never_reset(self):
        controller = self.controller()
        controller.ready.return_value = True
        self.assertEqual(usb.recover(controller), 0)
        controller.wait.assert_not_called()
        controller.reset_role.assert_not_called()

    def test_slow_enumeration_is_allowed_without_reset(self):
        controller = self.controller()
        controller.wait.return_value = True
        self.assertEqual(usb.recover(controller), 0)
        controller.reset_role.assert_not_called()

    def test_recovers_once_then_stops(self):
        controller = self.controller()
        controller.wait.side_effect = [False, True]
        self.assertEqual(usb.recover(controller), 0)
        controller.reset_role.assert_called_once()

    def test_failure_is_bounded_and_nonzero(self):
        controller = self.controller()
        self.assertEqual(usb.recover(controller), 1)
        self.assertEqual(controller.reset_role.call_count, 3)

    def test_check_only_never_waits_or_resets(self):
        controller = self.controller()
        self.assertEqual(usb.recover(controller, check_only=True), 1)
        controller.wait.assert_not_called()
        controller.reset_role.assert_not_called()

    def test_host_restored_even_when_interrupted(self):
        controller = usb.Controller('TEST')
        with patch.object(controller, 'ready', return_value=False), \
                patch.object(usb, 'ROLE') as role, \
                patch.object(usb, 'USB_PORT') as port, \
                patch.object(usb.time, 'sleep', side_effect=RuntimeError('terminated')):
            role.read_text.return_value = 'host\n'
            port.exists.return_value = False
            with self.assertRaises(RuntimeError):
                controller.reset_role()
            self.assertEqual([call.args[0] for call in role.write_text.call_args_list],
                             ['none\n', 'host\n'])

    def test_wrong_role_or_device_prevents_reset(self):
        for current_role in ('device', 'none', 'host'):
            with self.subTest(role=current_role):
                controller = usb.Controller('TEST')
                with patch.object(controller, 'ready', return_value=False), \
                        patch.object(usb, 'ROLE') as role, \
                        patch.object(usb, 'USB_PORT') as port:
                    role.read_text.return_value = current_role
                    port.exists.return_value = True
                    (port / 'serial').read_text.return_value = 'OTHER'
                    with self.assertRaises(RuntimeError):
                        controller.reset_role()
                    role.write_text.assert_not_called()

    def test_device_arriving_before_reset_is_untouched(self):
        controller = usb.Controller('TEST')
        with patch.object(controller, 'ready', return_value=True), \
                patch.object(usb, 'ROLE') as role:
            controller.reset_role()
            role.write_text.assert_not_called()
