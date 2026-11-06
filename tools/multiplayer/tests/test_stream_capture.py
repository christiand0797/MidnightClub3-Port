"""Synthetic GDI/PNG and bounded worker checks; no game, actual capture or GPU."""
import asyncio
import ctypes
from ctypes import wintypes
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "stream"))
import capture_backend as capture
from stream_proxy import StreamProxy, FFmpegEncoder


class FakeAPI:
    def __init__(self, width=3, height=2):
        self.width, self.height = width, height
        self.user32, self.gdi32 = Mock(), Mock()
        self.user32.GetDC.return_value = 0x123400001
        self.gdi32.CreateCompatibleDC.return_value = 0x123400002
        self.gdi32.SelectObject.return_value = 0x123400004
        self.user32.PrintWindow.return_value = 1
        self.user32.GetClientRect.side_effect = self.rect
        raw = bytes([3, 2, 1, 250, 6, 5, 4, 0, 9, 8, 7, 127]) * 2
        self.raw = ctypes.create_string_buffer(raw)
        self.gdi32.CreateDIBSection.side_effect = self.dib
        self.process = Mock(return_value=(77, "C:/fixture/mc3.exe", 1234))
        self.instance = Mock(return_value=7)
        self.enum_callback = lambda fn: fn

    def rect(self, hwnd, value):
        value._obj.left = value._obj.top = 0
        value._obj.right, value._obj.bottom = self.width, self.height
        return 1

    def dib(self, dc, info, usage, pixels, section, offset):
        self.info = info._obj.bmiHeader
        pixels._obj.value = ctypes.addressof(self.raw)
        return 0x123400003


def decode_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    at, compressed, width, height = 8, b"", 0, 0
    while at < len(data):
        length = struct.unpack_from(">I", data, at)[0]
        kind, payload = data[at+4:at+8], data[at+8:at+8+length]
        assert zlib.crc32(kind + payload) == struct.unpack_from(">I", data, at+8+length)[0]
        if kind == b"IHDR":
            width, height, depth, color, _, _, _ = struct.unpack(">2I5B", payload)
            assert (depth, color) == (8, 2)
        if kind == b"IDAT":
            compressed += payload
        at += length + 12
    rows = zlib.decompress(compressed)
    stride = width * 3
    assert all(rows[i] == 0 for i in range(0, len(rows), stride + 1))
    return width, height, b"".join(rows[i+1:i+1+stride] for i in range(0, len(rows), stride+1))


class CaptureTests(unittest.TestCase):
    def test_windows_struct_layout_and_pointer_width(self):
        self.assertEqual(ctypes.sizeof(capture.BITMAPINFOHEADER), 40)
        self.assertEqual(ctypes.sizeof(capture.BITMAPINFO), 44)
        if sys.platform == "win32":
            api = capture.Win32API()  # binds API prototypes only; does not capture/enumerate
            self.assertIs(api.gdi32.CreateDIBSection.restype, wintypes.HBITMAP)
            self.assertIs(api.gdi32.CreateCompatibleDC.restype, wintypes.HDC)
            self.assertIs(api.user32.GetDC.restype, wintypes.HDC)
            self.assertEqual(len(api.user32.PrintWindow.argtypes), 3)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows process metadata')
    def test_actual_isolated_child_command_line_and_creation_match(self):
        # Read process metadata only. No window, game, capture or GPU is involved.
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(4)', '--instance', '7'])
        api = capture.Win32API()
        handle = api.kernel32.OpenProcess(0x1000 | 0x100000, False, child.pid)
        try:
            self.assertTrue(handle)
            created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
            self.assertTrue(api.kernel32.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(kernel), ctypes.byref(user)))
            creation = created.dwLowDateTime | created.dwHighDateTime << 32
            self.assertEqual(api.instance(child.pid, creation), 7)
            self.assertIsNone(api.instance(child.pid, creation + 100000))
        finally:
            if handle:
                api.kernel32.CloseHandle(handle)
            child.wait(timeout=10)

    def test_top_down_32_bit_pixels_and_cleanup(self):
        api = FakeAPI()
        width, height, rgb = capture.capture_window(123, 77, api, expected_executable="C:/fixture/mc3.exe", expected_creation=1234)
        self.assertEqual((width, height), (3, 2))
        self.assertEqual(rgb, bytes(range(1, 10)) * 2)
        self.assertEqual((api.info.biWidth, api.info.biHeight, api.info.biBitCount), (3, -2, 32))
        self.assertEqual(api.gdi32.SelectObject.call_args_list[-1].args, (0x123400002, 0x123400004))
        api.gdi32.DeleteObject.assert_called_once_with(0x123400003)
        api.gdi32.DeleteDC.assert_called_once_with(0x123400002)
        api.user32.ReleaseDC.assert_called_once_with(123, 0x123400001)

    def test_capture_failure_releases_every_allocated_handle(self):
        api = FakeAPI()
        api.user32.PrintWindow.return_value = 0
        with self.assertRaises(capture.CaptureError):
            capture.capture_window(123, 77, api, expected_executable="C:/fixture/mc3.exe", expected_creation=1234)
        self.assertEqual(api.gdi32.SelectObject.call_count, 2)
        api.gdi32.DeleteObject.assert_called_once()
        api.gdi32.DeleteDC.assert_called_once()
        api.user32.ReleaseDC.assert_called_once()

    def test_bitmap_failure_releases_contexts(self):
        api = FakeAPI()
        api.gdi32.CreateDIBSection.side_effect = None
        api.gdi32.CreateDIBSection.return_value = 0
        with self.assertRaises(capture.CaptureError):
            capture.capture_window(123, 77, api, expected_executable="C:/fixture/mc3.exe", expected_creation=1234)
        api.gdi32.DeleteObject.assert_not_called()
        api.gdi32.DeleteDC.assert_called_once()
        api.user32.ReleaseDC.assert_called_once()

    def test_selected_object_failure_releases_bitmap(self):
        api = FakeAPI()
        api.gdi32.SelectObject.return_value = ctypes.c_void_p(-1).value
        with self.assertRaises(capture.CaptureError):
            capture.capture_window(123, 77, api, expected_executable="C:/fixture/mc3.exe", expected_creation=1234)
        api.gdi32.DeleteObject.assert_called_once()
        api.gdi32.DeleteDC.assert_called_once()
        api.user32.ReleaseDC.assert_called_once()

    def test_recycled_window_and_foreign_process_rejected_before_capture(self):
        for identity in ((88, "C:/fixture/mc3.exe"), (77, "C:/browser/chrome.exe"), None):
            api = FakeAPI()
            api.process.return_value = identity
            with self.subTest(identity=identity), self.assertRaises(capture.CaptureError):
                capture.capture_window(123, 77, api, expected_executable="C:/fixture/mc3.exe", expected_creation=1234)
            api.user32.GetDC.assert_not_called()

    def test_same_exe_different_install_and_reused_pid_are_rejected(self):
        for identity in ((77, "C:/other-install/mc3.exe", 1234), (77, "C:/fixture/mc3.exe", 9999)):
            api = FakeAPI()
            api.process.return_value = identity
            with self.subTest(identity=identity), self.assertRaises(capture.CaptureError):
                capture.capture_window(123, 77, api, expected_executable="C:/fixture/mc3.exe", expected_creation=1234)
            api.user32.GetDC.assert_not_called()

    def test_window_owner_change_during_capture_discards_pixels_and_cleans_up(self):
        api = FakeAPI()
        api.process.side_effect = [(77, "C:/fixture/mc3.exe", 1234), (77, "C:/fixture/mc3.exe", 9999)]
        with self.assertRaises(capture.CaptureError):
            capture.capture_window(123, 77, api, expected_executable="C:/fixture/mc3.exe", expected_creation=1234)
        api.gdi32.DeleteObject.assert_called_once()
        api.user32.ReleaseDC.assert_called_once()

    def test_window_selection_checks_actual_exe_pid_creation_and_command_line_instance(self):
        api = FakeAPI()
        title = "Midnight Club 3: DUB Edition Remix [7]"
        api.user32.IsWindowVisible.return_value = 1
        api.user32.GetWindowTextLengthW.return_value = len(title)
        def text(hwnd, buffer, size):
            buffer.value = title
            return len(title)
        api.user32.GetWindowTextW.side_effect = text
        api.user32.EnumWindows.side_effect = lambda callback, param: callback(123, param)
        result = capture.find_window(instance=7, api=api, expected_executable="C:/fixture/mc3.exe", expected_pid=77)
        self.assertEqual((result['pid'], result['creation']), (77, 1234))
        api.instance.assert_called_once_with(77, 1234)
        api.instance.return_value = 2
        self.assertIsNone(capture.find_window(instance=7, api=api, expected_executable="C:/fixture/mc3.exe", expected_pid=77))
        api.instance.return_value = None
        self.assertIsNone(capture.find_window(instance=7, api=api, expected_executable="C:/fixture/mc3.exe", expected_pid=77))
        api.instance.return_value = 7
        self.assertIsNone(capture.find_window(instance=7, api=api, expected_executable="C:/other/mc3.exe", expected_pid=77))
        self.assertIsNone(capture.find_window(instance=7, api=api, expected_executable="C:/fixture/mc3.exe", expected_pid=88))
        api.process.return_value = (77, "C:/fixture/mc3.exe", 0)
        self.assertIsNone(capture.find_window(instance=7, api=api, expected_executable="C:/fixture/mc3.exe", expected_pid=77))

    def test_target_is_explicit_absolute_game_executable(self):
        for target in (None, "mc3.exe", "C:/fixture/chrome.exe"):
            with self.subTest(target=target), self.assertRaises(capture.CaptureError):
                capture.find_window(expected_executable=target, api=FakeAPI())

    def test_bounds_before_any_bitmap_allocation(self):
        for width, height in ((0, 2), (9000, 2), (8192, 8192), (-1, 2)):
            api = FakeAPI(width, height)
            with self.subTest(size=(width, height)), self.assertRaises(capture.CaptureError):
                capture.capture_window(123, 77, api, expected_executable="C:/fixture/mc3.exe", expected_creation=1234)
            api.user32.GetDC.assert_not_called()

    def test_png_pixels_crc_and_odd_width_round_trip(self):
        rgb = bytes(range(1, 28))
        self.assertEqual(decode_png(capture.encode_png(3, 3, rgb)), (3, 3, rgb))

    def test_png_bad_frame_is_rejected(self):
        for args in ((2, 2, b"x", 1), (2, 2, b"x" * 12, 0), (0, 2, b"", 1)):
            with self.subTest(args=args), self.assertRaises(capture.CaptureError):
                capture.encode_png(*args)

    def test_mss_screenshot_rgb_api_without_any_real_capture(self):
        from mss.screenshot import ScreenShot
        image = ScreenShot(bytearray([3, 2, 1, 0] * 6), {"left": 0, "top": 0, "width": 3, "height": 2})
        context = Mock()
        context.__enter__ = Mock(return_value=context)
        context.__exit__ = Mock(return_value=False)
        context.grab.return_value = image
        proxy = StreamProxy.__new__(StreamProxy)
        proxy.capture_window, proxy.monitor, proxy.scale = False, {"width": 3, "height": 2}, 1.0
        with patch("stream_proxy.mss.mss", return_value=context):
            kind, data = proxy.capture_frame()
        self.assertEqual(kind, "png")
        self.assertEqual(decode_png(data), (3, 2, bytes([1, 2, 3] * 6)))
        context.__exit__.assert_called_once()

    def test_window_not_found_does_not_capture_monitor(self):
        with tempfile.TemporaryDirectory(prefix="mc3-capture-target-fixture-") as temporary:
            executable = Path(temporary) / "mc3.exe"
            executable.write_bytes(b"unexecutable fixture target")
            proxy = StreamProxy.__new__(StreamProxy)
            proxy.capture_window, proxy.pad_addr = True, ("127.0.0.1", 30778)
            proxy.game_executable, proxy.game_pid = executable, 77
            with patch("stream_proxy._find_mc3_window", return_value=None) as find, patch("stream_proxy.mss.mss") as monitor:
                self.assertFalse(proxy.setup_capture())
            find.assert_called_once_with("Midnight Club 3", 7, executable, 77)
            monitor.assert_not_called()

    def test_h264_never_starts_encoder_or_claims_a_frame(self):
        with patch("subprocess.Popen") as process:
            encoder = FFmpegEncoder(1920, 1080, 60, "h264_nvenc")
            self.assertIsNone(encoder.proc)
            self.assertEqual(encoder.encode(b"fixture"), b"")
            encoder.close()
            process.assert_not_called()

    def test_worker_one_capture_in_flight_and_one_result(self):
        started, release = threading.Event(), threading.Event()
        count = []
        def blocking():
            count.append(1)
            started.set()
            release.wait(3)
            return ("png", b"fixture")
        worker = capture.CaptureWorker(blocking)
        try:
            self.assertTrue(worker.request())
            self.assertTrue(started.wait(1))
            self.assertTrue(all(not worker.request() for _ in range(100)))
            self.assertEqual(count, [1])
            self.assertIsNone(worker.poll())
            release.set()
            deadline = time.monotonic() + 2
            value = None
            while value is None and time.monotonic() < deadline:
                value = worker.poll()
                time.sleep(0.002)
            self.assertEqual(value, (True, ("png", b"fixture")))
            self.assertIsNone(worker.poll())
        finally:
            release.set()
            worker.close()

    def test_worker_reports_exception_and_remains_bounded(self):
        worker = capture.CaptureWorker(lambda: (_ for _ in ()).throw(ValueError("synthetic failure")))
        try:
            self.assertTrue(worker.request())
            deadline, result = time.monotonic() + 2, None
            while result is None and time.monotonic() < deadline:
                result = worker.poll()
                time.sleep(0.002)
            self.assertEqual(result, (False, "synthetic failure"))
            self.assertEqual(worker.results.maxsize, 1)
        finally:
            worker.close()


class LoopTests(unittest.IsolatedAsyncioTestCase):
    async def test_slow_capture_does_not_block_async_controller_loop(self):
        proxy = StreamProxy.__new__(StreamProxy)
        release, entered = threading.Event(), threading.Event()
        def blocked():
            entered.set()
            release.wait(2)
            return None
        proxy.capture_frame = blocked
        proxy.capture_window, proxy.fps, proxy.scale = True, 60, 1
        proxy.running, proxy.ws_clients = True, {"synthetic-viewer"}
        proxy.last_frame_time, proxy.frame_interval = 0, 1 / 60
        proxy.frame_count = proxy.bytes_sent = 0
        task = asyncio.create_task(proxy.capture_loop())
        ticks = 0
        try:
            for _ in range(15):
                await asyncio.sleep(0.005)
                ticks += 1
            self.assertTrue(entered.is_set())
            self.assertEqual(ticks, 15)
            self.assertFalse(task.done())
        finally:
            proxy.running = False
            release.set()
            await asyncio.wait_for(task, 1)


if __name__ == "__main__":
    unittest.main()
