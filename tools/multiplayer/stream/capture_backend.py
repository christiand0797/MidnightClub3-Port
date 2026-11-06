"""Bounded, window-only Win32 capture and lossless PNG encoding.

No capture occurs at import. Tests inject fake APIs; the live path accepts only
an mc3.exe process and never broadens a missing game window into desktop sharing.
"""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import queue
import json
import re
import struct
import subprocess
import threading
import zlib

MAX_PIXELS = 16 * 1024 * 1024
MAX_DIMENSION = 8192


class CaptureError(RuntimeError):
    pass


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class RGBQUAD(ctypes.Structure):
    _fields_ = [("rgbBlue", wintypes.BYTE), ("rgbGreen", wintypes.BYTE),
                ("rgbRed", wintypes.BYTE), ("rgbReserved", wintypes.BYTE)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", RGBQUAD * 1)]


def dimensions(width, height):
    if (type(width) is not int or type(height) is not int or width <= 0 or height <= 0 or
            width > MAX_DIMENSION or height > MAX_DIMENSION or width * height > MAX_PIXELS):
        raise CaptureError("Game window dimensions are outside the capture budget.")


class Win32API:
    def __init__(self):
        if os.name != "nt":
            raise CaptureError("Game-window capture requires Windows.")
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        # Per-monitor DPI awareness: a DPI-unaware process reads a scaled-down client rect while PrintWindow
        # draws physical pixels, which crops and shifts the captured frame on displays above 100% scaling.
        try:
            self.user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
            if not self.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):  # PER_MONITOR_AWARE_V2
                ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
        except (AttributeError, OSError):
            pass
        self.enum_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        # Explicit pointer-width handle signatures avoid ctypes' default C-int truncation.
        signatures = [
            (self.user32.EnumWindows, [self.enum_callback, wintypes.LPARAM], wintypes.BOOL),
            (self.user32.IsWindowVisible, [wintypes.HWND], wintypes.BOOL),
            (self.user32.GetWindowTextLengthW, [wintypes.HWND], ctypes.c_int),
            (self.user32.GetWindowTextW, [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int], ctypes.c_int),
            (self.user32.GetClientRect, [wintypes.HWND, ctypes.POINTER(wintypes.RECT)], wintypes.BOOL),
            (self.user32.GetWindowThreadProcessId, [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)], wintypes.DWORD),
            (self.user32.GetDC, [wintypes.HWND], wintypes.HDC),
            (self.user32.ReleaseDC, [wintypes.HWND, wintypes.HDC], ctypes.c_int),
            (self.user32.PrintWindow, [wintypes.HWND, wintypes.HDC, wintypes.UINT], wintypes.BOOL),
            (self.gdi32.CreateCompatibleDC, [wintypes.HDC], wintypes.HDC),
            (self.gdi32.CreateDIBSection, [wintypes.HDC, ctypes.POINTER(BITMAPINFO), wintypes.UINT,
                                          ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD], wintypes.HBITMAP),
            (self.gdi32.SelectObject, [wintypes.HDC, wintypes.HANDLE], wintypes.HANDLE),
            (self.gdi32.DeleteObject, [wintypes.HANDLE], wintypes.BOOL),
            (self.gdi32.DeleteDC, [wintypes.HDC], wintypes.BOOL),
            (self.kernel32.OpenProcess, [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            (self.kernel32.QueryFullProcessImageNameW, [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                        ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            (self.kernel32.CloseHandle, [wintypes.HANDLE], wintypes.BOOL),
            (self.kernel32.GetProcessTimes, [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
                                            ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME)], wintypes.BOOL),
            (self.kernel32.WaitForSingleObject, [wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
        ]
        for function, arguments, result in signatures:
            function.argtypes, function.restype = arguments, result

    def process(self, hwnd):
        pid = wintypes.DWORD()
        if not self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid)) or not pid.value:
            return None
        handle = self.kernel32.OpenProcess(0x1000 | 0x100000, False, pid.value)
        if not handle:
            return None
        try:
            count = wintypes.DWORD(32768)
            image = ctypes.create_unicode_buffer(count.value)
            if not self.kernel32.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(count)):
                return None
            created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
            if not self.kernel32.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(kernel), ctypes.byref(user)):
                return None
            if self.kernel32.WaitForSingleObject(handle, 0) != 0x102:
                return None
            return (pid.value, image.value, created.dwLowDateTime | created.dwHighDateTime << 32)
        finally:
            self.kernel32.CloseHandle(handle)

    def instance(self, pid, creation):
        # Run only when selecting the target, never per frame. CIM's timestamp
        # precision is microseconds; the Win32 creation identity remains 100 ns.
        script = ("$ErrorActionPreference='Stop'; [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); "
                  f"Get-CimInstance Win32_Process -Filter 'ProcessId = {int(pid)}' | "
                  "Select-Object CommandLine,@{Name='Creation';Expression={$_.CreationDate.ToFileTimeUtc().ToString()}} | ConvertTo-Json -Compress")
        powershell = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        try:
            result = subprocess.run([str(powershell), "-NoProfile", "-NonInteractive", "-Command", script],
                                    capture_output=True, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
        except (OSError, subprocess.SubprocessError):
            return None
        if result.returncode:
            return None
        try:
            row = json.loads(result.stdout.decode("utf-8-sig"))
            if not isinstance(row, dict) or int(row.get("Creation", -1)) // 10 != creation // 10 or not isinstance(row.get("CommandLine"), str):
                return None
            # Exact original main.cpp substring + _wtoi behavior, clamped 0..8.
            number = re.match(r"\s*([+-]?[0-9]+)", row["CommandLine"].partition("--instance")[2])
            return max(0, min(8, int(number[1]))) if number else 0
        except (ValueError, UnicodeError):
            return None


def target_path(value):
    if value is None:
        raise CaptureError("An exact game executable path is required.")
    path = Path(value)
    if not path.is_absolute() or path.name.lower() != "mc3.exe":
        raise CaptureError("Select an absolute mc3.exe executable path.")
    return os.path.normcase(str(path.resolve()))


def matches_process(identity, executable, pid=None, creation=None):
    return (identity is not None and len(identity) == 3 and identity[2] > 0 and
            os.path.normcase(str(Path(identity[1]).resolve())) == executable and
            (pid is None or identity[0] == pid) and (creation is None or identity[2] == creation))


def find_window(partial_title="Midnight Club 3", instance=0, api=None, *, expected_executable=None, expected_pid=None):
    executable = target_path(expected_executable)
    if not 0 <= instance <= 8 or (expected_pid is not None and (type(expected_pid) is not int or expected_pid <= 0)):
        raise CaptureError("Invalid selected native instance/PID.")
    api = api or Win32API()
    found = []
    def visit(hwnd, _):
        if not api.user32.IsWindowVisible(hwnd):
            return True
        length = api.user32.GetWindowTextLengthW(hwnd)
        if not 0 < length <= 4096:
            return True
        text = ctypes.create_unicode_buffer(length + 1)
        if not api.user32.GetWindowTextW(hwnd, text, length + 1) or partial_title.casefold() not in text.value.casefold():
            return True
        # Native instance 0 has no suffix; explicit instances use [N] in main.cpp.
        if (instance and not text.value.endswith(f" [{instance}]")) or (not instance and text.value.endswith(tuple(f" [{i}]" for i in range(1, 9)))):
            return True
        identity = api.process(hwnd)
        if not matches_process(identity, executable, expected_pid):
            return True
        if api.instance(identity[0], identity[2]) != instance:
            return True
        rect = wintypes.RECT()
        if not api.user32.GetClientRect(hwnd, ctypes.byref(rect)):
            return True
        width, height = rect.right - rect.left, rect.bottom - rect.top
        try:
            dimensions(width, height)
        except CaptureError:
            return True
        found.append({"hwnd": hwnd, "width": width, "height": height, "title": text.value,
                      "pid": identity[0], "creation": identity[2]})
        return False
    callback = api.enum_callback(visit)
    api.user32.EnumWindows(callback, 0)
    return found[0] if found else None


def capture_window(hwnd, expected_pid=None, api=None, *, expected_executable=None, expected_creation=None):
    executable = target_path(expected_executable)
    if type(expected_pid) is not int or expected_pid <= 0 or type(expected_creation) is not int or expected_creation <= 0:
        raise CaptureError("A checked game process/creation identity is required.")
    api = api or Win32API()
    identity = api.process(hwnd)
    if not matches_process(identity, executable, expected_pid, expected_creation):
        raise CaptureError("The selected game window no longer belongs to the checked MC3 process.")
    rect = wintypes.RECT()
    if not api.user32.GetClientRect(hwnd, ctypes.byref(rect)):
        raise CaptureError("Cannot read the selected game client area.")
    width, height = rect.right - rect.left, rect.bottom - rect.top
    dimensions(width, height)
    window_dc = memory_dc = bitmap = old_bitmap = None
    try:
        window_dc = api.user32.GetDC(hwnd)
        if not window_dc:
            raise CaptureError("Cannot get the game window device context.")
        memory_dc = api.gdi32.CreateCompatibleDC(window_dc)
        if not memory_dc:
            raise CaptureError("Cannot create the capture device context.")
        info = BITMAPINFO()
        info.bmiHeader = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), width, -height, 1, 32, 0,
                                          width * height * 4, 0, 0, 0, 0)
        pixels = ctypes.c_void_p()
        bitmap = api.gdi32.CreateDIBSection(window_dc, ctypes.byref(info), 0, ctypes.byref(pixels), None, 0)
        if not bitmap or not pixels.value:
            raise CaptureError("Cannot allocate the bounded capture bitmap.")
        old_bitmap = api.gdi32.SelectObject(memory_dc, bitmap)
        if not old_bitmap or old_bitmap == ctypes.c_void_p(-1).value:
            old_bitmap = None
            raise CaptureError("Cannot select the capture bitmap.")
        # PW_CLIENTONLY | PW_RENDERFULLCONTENT: without the second flag a D3D12 (flip-model) window reads back black
        if not api.user32.PrintWindow(hwnd, memory_dc, 3):  # no desktop capture
            raise CaptureError("The game window did not provide a client-area capture.")
        if not matches_process(api.process(hwnd), executable, expected_pid, expected_creation):
            raise CaptureError("Game window ownership changed during capture.")
        raw = ctypes.string_at(pixels.value, width * height * 4)
        rgb = bytearray(width * height * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
        return width, height, bytes(rgb)
    finally:
        # Restore the previous selection before deleting a selected GDI bitmap.
        if old_bitmap:
            api.gdi32.SelectObject(memory_dc, old_bitmap)
        if bitmap:
            api.gdi32.DeleteObject(bitmap)
        if memory_dc:
            api.gdi32.DeleteDC(memory_dc)
        if window_dc:
            api.user32.ReleaseDC(hwnd, window_dc)


def encode_png(width, height, rgb, scale=1.0):
    dimensions(width, height)
    if len(rgb) != width * height * 3 or not 0.25 <= scale <= 1.0:
        raise CaptureError("Invalid RGB frame or requested scale.")
    if scale != 1.0:
        try:
            from PIL import Image
        except ImportError as error:
            raise CaptureError("Pillow is required for scaling; native scale 1 needs no Pillow.") from error
        image = Image.frombytes("RGB", (width, height), rgb)
        width, height = max(1, round(width * scale)), max(1, round(height * scale))
        rgb = image.resize((width, height), Image.Resampling.LANCZOS).tobytes()
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    compressor = zlib.compressobj(level=1)
    compressed = bytearray()
    stride = width * 3
    for offset in range(0, len(rgb), stride):
        compressed.extend(compressor.compress(b"\0" + rgb[offset:offset + stride]))
    compressed.extend(compressor.flush())
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", bytes(compressed)) + chunk(b"IEND", b""))


class CaptureWorker:
    """At most one capture in flight, one bounded result, and one daemon worker.

    A hung PrintWindow cannot stop the browser/control event loop or cause an
    unbounded series of replacement workers. close() does not terminate windows.
    """
    def __init__(self, capture):
        self.capture = capture
        self.requests, self.results = queue.Queue(1), queue.Queue(1)
        self.stopping = threading.Event()
        self.lock, self.busy = threading.Lock(), False
        self.thread = threading.Thread(target=self._run, name="mc3-window-capture", daemon=True)
        self.thread.start()

    def request(self):
        with self.lock:
            if self.busy or self.stopping.is_set() or not self.results.empty():
                return False
            self.busy = True
        self.requests.put_nowait(True)
        return True

    def poll(self):
        try:
            return self.results.get_nowait()
        except queue.Empty:
            return None

    def _run(self):
        while not self.stopping.is_set():
            if not self.requests.get():
                break
            try:
                result = (True, self.capture())
            except Exception as error:
                result = (False, str(error))
            if not self.stopping.is_set():
                self.results.put_nowait(result)
            with self.lock:
                self.busy = False

    def close(self):
        self.stopping.set()
        try:
            self.requests.put_nowait(False)
        except queue.Full:
            pass
        self.thread.join(timeout=0.2)
