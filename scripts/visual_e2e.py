#!/usr/bin/env python3
"""Capture and check the real FeatherApi.exe UI on an interactive Windows desktop.

The test launches the production executable with isolated JSON files. Screenshots
come from the physical desktop DC via GDI BitBlt when available, with Win32
PrintWindow of the production HWND as a fallback; no UI renderer, widget, HTTP
client, or application function is replaced. It verifies the actual monitor DPI
and saves PNG evidence plus a machine-readable report under build/real-visual/.

Run after a Release build: python scripts/visual_e2e.py
The desktop must be unlocked and the test window must be allowed to take focus.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import traceback
from urllib.parse import urlsplit
import uuid
import zlib

from real_e2e import (
    E2EFailure, IDC_RESPONSE_FIND, IDC_RESPONSE_FIND_CLOSE,
    Win32Window, force_stop, launch, require, require_isolated_workspace,
    seed_data, stop, wait_http,
)


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")


# Sequential IDs from src/ui/main_window.cpp. The production HWNDs are queried
# directly, rather than creating stand-ins for any controls.
IDC_SEARCH = 100
IDC_ADD_FOLDER = 101
IDC_TREE = 102
IDC_REQUEST_TABS = 103
IDC_METHOD = 104
IDC_URL = 105
IDC_SAVE = 106
IDC_SAVE_MORE = 107
IDC_SEND = 108
IDC_EDITOR_TABS = 110
IDC_KV_LIST = 111
IDC_SUMMARY = 119
IDC_RESPONSE_TABS = 120
IDC_RESPONSE_BODY = 121
IDC_RESPONSE_HEADERS = 122
IDC_RESPONSE_FIND_PANEL = 123
IDC_RESPONSE_FIND_EDIT = 124
IDC_RESPONSE_FIND_PREV = 125
IDC_RESPONSE_FIND_NEXT = 126
IDC_FIND_STATUS = 135
IDC_EMPTY_TITLE = 129
IDC_EMPTY_HELP = 130
IDC_RESPONSE_MODE = 133
IDC_EMPTY_NEW_REQUEST = 137
IDC_URL_FRAME = 901
IDC_SEARCH_FRAME = 902

TCM_GETCURSEL = 0x130B
LVM_GETITEMCOUNT = 0x1004
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
SW_RESTORE = 9
HWND_TOPMOST = -1
SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0
MONITORINFOF_PRIMARY = 1


class Rect(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG),
                ("right", wintypes.LONG), ("bottom", wintypes.LONG)]

    @property
    def width(self) -> int:
        return self.right - self.left

    @property
    def height(self) -> int:
        return self.bottom - self.top

    def as_list(self) -> list[int]:
        return [self.left, self.top, self.right, self.bottom]


class Point(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class MonitorInfo(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", Rect),
                ("rcWork", Rect), ("dwFlags", wintypes.DWORD)]


class BitmapInfoHeader(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class BitmapInfo(ctypes.Structure):
    _fields_ = [("bmiHeader", BitmapInfoHeader), ("bmiColors", wintypes.DWORD * 3)]


class Native:
    def __init__(self):
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        u, g = self.user32, self.gdi32
        u.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(Rect)]
        u.GetWindowRect.restype = wintypes.BOOL
        u.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
        u.EnumWindows.restype = wintypes.BOOL
        u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        u.GetWindowThreadProcessId.restype = wintypes.DWORD
        u.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        u.GetClassNameW.restype = ctypes.c_int
        u.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        u.GetWindowTextW.restype = ctypes.c_int
        u.IsWindowVisible.argtypes = [wintypes.HWND]
        u.IsWindowVisible.restype = wintypes.BOOL
        u.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(Rect)]
        u.GetClientRect.restype = wintypes.BOOL
        u.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(Point)]
        u.ClientToScreen.restype = wintypes.BOOL
        u.GetDpiForWindow.argtypes = [wintypes.HWND]
        u.GetDpiForWindow.restype = wintypes.UINT
        u.GetForegroundWindow.argtypes = []
        u.GetForegroundWindow.restype = wintypes.HWND
        u.SetForegroundWindow.argtypes = [wintypes.HWND]
        u.SetForegroundWindow.restype = wintypes.BOOL
        u.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        u.ShowWindow.restype = wintypes.BOOL
        u.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
        u.SetWindowPos.restype = wintypes.BOOL
        u.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.c_void_p,
                                           ctypes.c_void_p, wintypes.LPARAM]
        u.EnumDisplayMonitors.restype = wintypes.BOOL
        u.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MonitorInfo)]
        u.GetMonitorInfoW.restype = wintypes.BOOL
        u.GetDC.argtypes = [wintypes.HWND]
        u.GetDC.restype = wintypes.HDC
        u.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
        u.ReleaseDC.restype = ctypes.c_int
        u.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
        u.PrintWindow.restype = wintypes.BOOL
        g.CreateCompatibleDC.argtypes = [wintypes.HDC]
        g.CreateCompatibleDC.restype = wintypes.HDC
        g.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.POINTER(BitmapInfo),
                                       wintypes.UINT, ctypes.POINTER(ctypes.c_void_p),
                                       wintypes.HANDLE, wintypes.DWORD]
        g.CreateDIBSection.restype = wintypes.HBITMAP
        g.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
        g.SelectObject.restype = wintypes.HGDIOBJ
        g.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_int, wintypes.HDC,
                             ctypes.c_int, ctypes.c_int, wintypes.DWORD]
        g.BitBlt.restype = wintypes.BOOL
        g.DeleteObject.argtypes = [wintypes.HGDIOBJ]
        g.DeleteObject.restype = wintypes.BOOL
        g.DeleteDC.argtypes = [wintypes.HDC]
        g.DeleteDC.restype = wintypes.BOOL

    def window_rect(self, hwnd: int) -> Rect:
        rect = Rect()
        require(bool(self.user32.GetWindowRect(hwnd, ctypes.byref(rect))),
                f"GetWindowRect 失败: {ctypes.get_last_error()}")
        return rect

    def client_rect_on_screen(self, hwnd: int) -> Rect:
        rect = Rect()
        point = Point()
        require(bool(self.user32.GetClientRect(hwnd, ctypes.byref(rect))) and
                bool(self.user32.ClientToScreen(hwnd, ctypes.byref(point))),
                f"无法获取窗口客户区: {ctypes.get_last_error()}")
        return Rect(point.x, point.y, point.x + rect.width, point.y + rect.height)

    def monitors(self) -> list[dict]:
        found: list[dict] = []
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE,
                                           wintypes.HDC, ctypes.POINTER(Rect),
                                           wintypes.LPARAM)

        def add(handle, _hdc, _rect, _user):
            info = MonitorInfo()
            info.cbSize = ctypes.sizeof(info)
            if self.user32.GetMonitorInfoW(handle, ctypes.byref(info)):
                found.append({"handle": int(handle), "bounds": info.rcMonitor,
                              "work": info.rcWork,
                              "primary": bool(info.dwFlags & MONITORINFOF_PRIMARY)})
            return True

        callback = callback_type(add)
        require(bool(self.user32.EnumDisplayMonitors(None, None, callback, 0)),
                f"EnumDisplayMonitors 失败: {ctypes.get_last_error()}")
        require(bool(found), "找不到可用显示器")
        found.sort(key=lambda m: (not m["primary"], m["work"].left))
        return found

    def occluders_above(self, hwnd: int) -> list[dict]:
        """Visible top-level HWNDs above and intersecting the tested window."""
        target = self.window_rect(hwnd)
        found: list[dict] = []
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND,
                                           wintypes.LPARAM)

        def inspect(other, _unused):
            if other == hwnd:
                return False  # EnumWindows visits top-level windows in Z order.
            if not self.user32.IsWindowVisible(other):
                return True
            bounds = Rect()
            if not self.user32.GetWindowRect(other, ctypes.byref(bounds)):
                return True
            if (bounds.left >= target.right or bounds.right <= target.left or
                    bounds.top >= target.bottom or bounds.bottom <= target.top):
                return True
            pid = wintypes.DWORD()
            self.user32.GetWindowThreadProcessId(other, ctypes.byref(pid))
            class_name = ctypes.create_unicode_buffer(128)
            title = ctypes.create_unicode_buffer(512)
            self.user32.GetClassNameW(other, class_name, len(class_name))
            self.user32.GetWindowTextW(other, title, len(title))
            found.append({"hwnd": int(other), "pid": pid.value,
                          "class": class_name.value, "title": title.value,
                          "rect": bounds.as_list()})
            return True

        callback = callback_type(inspect)
        self.user32.EnumWindows(callback, 0)
        return found

    def place(self, hwnd: int, work: Rect, width: int, height: int) -> None:
        self.user32.ShowWindow(hwnd, SW_RESTORE)
        x = work.left + max(0, (work.width - width) // 2)
        y = work.top + max(0, (work.height - height) // 2)
        # Automation hosts may retain foreground activation permission. Put
        # the test window above ordinary windows so BitBlt still sees its real
        # desktop pixels; the window is closed at the end of the run.
        require(bool(self.user32.SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, 0)),
                f"SetWindowPos 失败: {ctypes.get_last_error()}")
        self.user32.SetForegroundWindow(hwnd)
        time.sleep(0.35)
        rect = self.window_rect(hwnd)
        require(rect.left >= work.left and rect.top >= work.top and
                rect.right <= work.right and rect.bottom <= work.bottom,
                f"生产窗口未完整位于显示器工作区: {rect.as_list()} / {work.as_list()}")

    def capture(self, hwnd: int, destination: Path,
                force_print: bool = False) -> "Screenshot":
        rect = self.window_rect(hwnd)
        require(rect.width > 0 and rect.height > 0, "生产窗口尺寸无效")
        screen_dc = self.user32.GetDC(None)
        require(bool(screen_dc), f"GetDC 失败: {ctypes.get_last_error()}")
        memory_dc = None
        bitmap = None
        previous = None
        try:
            memory_dc = self.gdi32.CreateCompatibleDC(screen_dc)
            require(bool(memory_dc), f"CreateCompatibleDC 失败: {ctypes.get_last_error()}")
            info = BitmapInfo()
            info.bmiHeader.biSize = ctypes.sizeof(BitmapInfoHeader)
            info.bmiHeader.biWidth = rect.width
            info.bmiHeader.biHeight = -rect.height  # top-down DIB, physical screen pixels
            info.bmiHeader.biPlanes = 1
            info.bmiHeader.biBitCount = 32
            pixels = ctypes.c_void_p()
            bitmap = self.gdi32.CreateDIBSection(screen_dc, ctypes.byref(info),
                                                  DIB_RGB_COLORS, ctypes.byref(pixels),
                                                  None, 0)
            require(bool(bitmap) and bool(pixels.value),
                    f"CreateDIBSection 失败: {ctypes.get_last_error()}")
            previous = self.gdi32.SelectObject(memory_dc, bitmap)
            require(bool(previous), f"SelectObject 失败: {ctypes.get_last_error()}")
            copied = not force_print and bool(self.gdi32.BitBlt(
                memory_dc, 0, 0, rect.width, rect.height,
                screen_dc, rect.left, rect.top, SRCCOPY))
            bitblt_error = ctypes.get_last_error() if not copied else 0
            method = "physical desktop GetDC(NULL)+BitBlt"
            if not copied:
                # Some managed desktop sessions deny reading the screen DC even
                # though the app is visible. PrintWindow asks the real HWND to
                # render its actual window and child controls into the DIB.
                require(bool(self.user32.PrintWindow(hwnd, memory_dc, 2)),
                        f"BitBlt 失败 ({bitblt_error})，PrintWindow 也失败 "
                        f"({ctypes.get_last_error()})")
                method = "production HWND PrintWindow(PW_RENDERFULLCONTENT)"
            bgra = ctypes.string_at(pixels, rect.width * rect.height * 4)
        finally:
            if previous:
                self.gdi32.SelectObject(memory_dc, previous)
            if bitmap:
                self.gdi32.DeleteObject(bitmap)
            if memory_dc:
                self.gdi32.DeleteDC(memory_dc)
            self.user32.ReleaseDC(None, screen_dc)
        shot = Screenshot(rect, bgra, method)
        shot.save_png(destination)
        return shot


class Screenshot:
    def __init__(self, rect: Rect, bgra: bytes, method: str):
        self.rect = rect
        self.bgra = bgra
        self.method = method

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        ix, iy = x - self.rect.left, y - self.rect.top
        require(0 <= ix < self.rect.width and 0 <= iy < self.rect.height,
                f"像素坐标超出截图范围: {(x, y)}")
        offset = (iy * self.rect.width + ix) * 4
        return self.bgra[offset + 2], self.bgra[offset + 1], self.bgra[offset]

    def count_color(self, rect: Rect, color: tuple[int, int, int],
                    tolerance: int = 6) -> int:
        # Count actual rendered pixels inside a real control, including
        # antialiased glyph interiors and RichEdit selection background.
        left = max(rect.left, self.rect.left) - self.rect.left
        right = min(rect.right, self.rect.right) - self.rect.left
        top = max(rect.top, self.rect.top) - self.rect.top
        bottom = min(rect.bottom, self.rect.bottom) - self.rect.top
        found = 0
        for y in range(top, bottom):
            base = (y * self.rect.width + left) * 4
            for x in range(left, right):
                offset = base + (x - left) * 4
                b, g, r = self.bgra[offset:offset + 3]
                if (abs(r - color[0]) <= tolerance and
                        abs(g - color[1]) <= tolerance and
                        abs(b - color[2]) <= tolerance):
                    found += 1
        return found

    def count_ink(self, rect: Rect) -> int:
        """Count foreground pixels inside an owner-drawn light button."""
        left = max(rect.left, self.rect.left) - self.rect.left
        right = min(rect.right, self.rect.right) - self.rect.left
        top = max(rect.top, self.rect.top) - self.rect.top
        bottom = min(rect.bottom, self.rect.bottom) - self.rect.top
        found = 0
        for y in range(top, bottom):
            base = (y * self.rect.width + left) * 4
            for x in range(left, right):
                offset = base + (x - left) * 4
                b, g, r = self.bgra[offset:offset + 3]
                if r + g + b < 550:
                    found += 1
        return found

    def save_png(self, path: Path) -> None:
        rows = bytearray()
        stride = self.rect.width * 4
        for y in range(self.rect.height):
            rows.append(0)  # PNG filter type None
            source = self.bgra[y * stride:(y + 1) * stride]
            for x in range(0, stride, 4):
                rows.extend((source[x + 2], source[x + 1], source[x]))

        def chunk(kind: bytes, data: bytes) -> bytes:
            return (struct.pack(">I", len(data)) + kind + data +
                    struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

        png = (b"\x89PNG\r\n\x1a\n" +
               chunk(b"IHDR", struct.pack(">IIBBBBB", self.rect.width,
                                             self.rect.height, 8, 2, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(bytes(rows), 6)) +
               chunk(b"IEND", b""))
        path.write_bytes(png)


def capture_worker(arguments: list[str]) -> int:
    """Capture a real HWND in a disposable process; exchange pixels via disk."""
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--hwnd", type=int, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--force-print", action="store_true")
    args = parser.parse_args(arguments)
    result = {"status": "running", "pid": os.getpid(), "hwnd": args.hwnd}
    try:
        args.result.write_text(json.dumps(result), encoding="utf-8")
        use_per_monitor_dpi()
        shot = Native().capture(args.hwnd, args.destination, args.force_print)
        # Large BGRA payloads must not block the parent's process wait on a
        # full pipe or multiprocessing queue. The parent reads only after exit.
        args.result.with_suffix(".bgra").write_bytes(shot.bgra)
        result.update(status="passed", rect=shot.rect.as_list(), method=shot.method)
        args.result.write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
        return 0
    except BaseException as error:
        result.update(status="failed", error=str(error), traceback=traceback.format_exc())
        args.result.write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
        traceback.print_exc()
        return 1


def capture_with_timeout(hwnd: int, destination: Path, timeout: float,
                         force_print: bool = False) -> Screenshot:
    """Bound BitBlt/PrintWindow and PNG encoding without replacing HWND drawing."""
    result_path = destination.with_name(
        f"{destination.stem}-capture-{uuid.uuid4().hex[:8]}.json")
    pixels_path = result_path.with_suffix(".bgra")
    log_path = result_path.with_suffix(".log")
    command = [sys.executable, str(Path(__file__).resolve()), "--capture-worker",
               "--hwnd", str(hwnd), "--destination", str(destination),
               "--result", str(result_path)]
    if force_print:
        command.append("--force-print")
    process = None
    try:
        with log_path.open("w", encoding="utf-8") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired as error:
                force_stop(process)
                result_path.write_text(json.dumps({
                    "status": "timed_out", "pid": process.pid, "hwnd": hwnd,
                    "timeout_seconds": timeout, "exit_code": process.returncode,
                    "log": str(log_path),
                }, ensure_ascii=False, indent=2), encoding="utf-8")
                raise E2EFailure(
                    f"真实窗口截图超过 {timeout:g} 秒，已终止截图辅助进程；"
                    f"诊断: {result_path}；日志: {log_path}") from error
        try:
            result = json.loads(result_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise E2EFailure(
                f"截图辅助进程未提供有效结果 (exit={process.returncode})；"
                f"日志: {log_path}") from error
        require(process.returncode == 0 and result.get("status") == "passed",
                f"真实窗口截图失败 (exit={process.returncode}): "
                f"{result.get('error', result.get('status'))}；"
                f"诊断: {result_path}；日志: {log_path}")
        rect = Rect(*result["rect"])
        pixels = pixels_path.read_bytes()
        require(rect.width > 0 and rect.height > 0 and
                len(pixels) == rect.width * rect.height * 4,
                f"截图辅助进程返回不完整像素数据；诊断: {result_path}")
        return Screenshot(rect, pixels, result["method"])
    finally:
        force_stop(process)
        pixels_path.unlink(missing_ok=True)


def dpi_scale(dpi: int, dips: int) -> int:
    return max(1, (dpi * dips + 48) // 96)


def inside(outer: Rect, inner: Rect, label: str, tolerance: int = 1) -> None:
    require(inner.width > 0 and inner.height > 0 and
            inner.left >= outer.left - tolerance and
            inner.top >= outer.top - tolerance and
            inner.right <= outer.right + tolerance and
            inner.bottom <= outer.bottom + tolerance,
            f"{label} 越出区域: {inner.as_list()} / {outer.as_list()}")


def separated(left: Rect, right: Rect, labels: str, tolerance: int = 1) -> None:
    require(left.right <= right.left + tolerance,
            f"{labels} 水平重叠: {left.as_list()} / {right.as_list()}")


def above(top: Rect, bottom: Rect, labels: str, tolerance: int = 1) -> None:
    require(top.bottom <= bottom.top + tolerance,
            f"{labels} 垂直重叠: {top.as_list()} / {bottom.as_list()}")


def color_near(actual: tuple[int, int, int], expected: tuple[int, int, int],
               label: str, tolerance: int = 8) -> None:
    require(all(abs(a - b) <= tolerance for a, b in zip(actual, expected)),
            f"{label} 像素颜色异常: 实际 {actual}, 预期 {expected} ±{tolerance}")


def control_rect(native: Native, window: Win32Window, control_id: int,
                 label: str) -> Rect:
    if control_id in (IDC_RESPONSE_FIND_EDIT, IDC_RESPONSE_FIND_PREV,
                      IDC_RESPONSE_FIND_NEXT, IDC_RESPONSE_FIND_CLOSE, IDC_FIND_STATUS):
        panel = window.control(IDC_RESPONSE_FIND_PANEL)
        handle = window.user32.GetDlgItem(panel, control_id)
        require(bool(handle), f"找不到真实响应查找控件 {control_id}")
    else:
        handle = window.control(control_id)
    require(bool(native.user32.IsWindowVisible(handle)), f"{label} 不可见")
    return native.window_rect(handle)


def selected_tab_underline(native: Native, window: Win32Window,
                           shot: Screenshot, control_id: int, label: str,
                           expected_index: int = 0) -> None:
    hwnd = window.control(control_id)
    index = window.send(hwnd, TCM_GETCURSEL)
    require(index == expected_index,
            f"{label} 第 {expected_index + 1} 个标签未被选中: {index}")
    # TCM_GETITEMRECT carries a pointer and cannot be sent from this Python
    # process to the production process without remote memory allocation.
    # These tabs have a fixed 68-DIP item width in the production layout.
    bounds = native.window_rect(hwnd)
    dpi = native.user32.GetDpiForWindow(window.hwnd)
    point = (bounds.left + dpi_scale(dpi, 68 * expected_index + 34),
             bounds.bottom - 1)
    color_near(shot.pixel(*point), (37, 99, 235), f"{label} 选中下划线")


def check_loaded_ui(native: Native, window: Win32Window, shot: Screenshot,
                    report: dict, label: str) -> None:
    client = native.client_rect_on_screen(window.hwnd)
    names = {
        "search_frame": IDC_SEARCH_FRAME, "search": IDC_SEARCH,
        "add_folder": IDC_ADD_FOLDER, "tree": IDC_TREE,
        "request_tabs": IDC_REQUEST_TABS, "method": IDC_METHOD,
        "url_frame": IDC_URL_FRAME, "url": IDC_URL,
        "save": IDC_SAVE, "save_more": IDC_SAVE_MORE, "send": IDC_SEND,
        "editor_tabs": IDC_EDITOR_TABS, "kv_list": IDC_KV_LIST,
        "summary": IDC_SUMMARY, "response_tabs": IDC_RESPONSE_TABS,
        "response_body": IDC_RESPONSE_BODY,
        "response_mode": IDC_RESPONSE_MODE, "response_find": IDC_RESPONSE_FIND,
    }
    rects = {name: control_rect(native, window, control_id, name)
             for name, control_id in names.items()}
    for name, rect in rects.items():
        inside(client, rect, name)
    inside(rects["search_frame"], rects["search"], "侧栏搜索输入框")
    inside(rects["url_frame"], rects["url"], "请求 URL 输入框")
    separated(rects["search_frame"], rects["add_folder"], "搜索框/新建按钮")
    separated(rects["tree"], rects["method"], "侧栏/请求工作区")
    separated(rects["method"], rects["url_frame"], "方法/URL")
    separated(rects["url_frame"], rects["send"], "URL/发送按钮")
    separated(rects["save"], rects["save_more"], "保存/更多按钮")
    separated(rects["response_tabs"], rects["response_mode"], "响应标签/模式")
    separated(rects["response_mode"], rects["response_find"], "响应模式/查找")
    above(rects["search_frame"], rects["tree"], "侧栏搜索/树")
    above(rects["request_tabs"], rects["method"], "请求标签/请求栏")
    above(rects["method"], rects["editor_tabs"], "请求栏/编辑标签")
    above(rects["editor_tabs"], rects["kv_list"], "编辑标签/参数列表")
    above(rects["kv_list"], rects["summary"], "参数列表/响应摘要")
    above(rects["summary"], rects["response_tabs"], "响应摘要/标签")
    above(rects["response_tabs"], rects["response_body"], "响应标签/正文")
    require(window.text(IDC_METHOD) == "GET", "预置真实请求的 GET 方法未显示")
    dpi = native.user32.GetDpiForWindow(window.hwnd)
    color_near(shot.pixel(client.left + dpi_scale(dpi, 3),
                          client.top + dpi_scale(dpi, 100)),
               (247, 248, 250), "侧栏背景")
    color_near(shot.pixel(rects["send"].left + dpi_scale(dpi, 8),
                          (rects["send"].top + rects["send"].bottom) // 2),
               (37, 99, 235), "发送按钮品牌蓝")
    color_near(shot.pixel(rects["method"].left + dpi_scale(dpi, 12),
                          (rects["method"].top + rects["method"].bottom) // 2),
               (225, 244, 235), "GET 方法绿色徽标")
    selected_tab_underline(native, window, shot, IDC_EDITOR_TABS, "编辑标签")
    selected_tab_underline(native, window, shot, IDC_RESPONSE_TABS, "响应标签")
    # WM_GETTEXT and IsWindowVisible can both pass while the owner-drawn Save
    # face is blank. Sample only its inset interior, away from the pale border
    # and the adjacent Save More arrow. The icon and label use dark/colored
    # foreground pixels in every legitimate state (normal, hover, saved).
    save = rects["save"]
    require(bool(window.text(IDC_SAVE).strip()), "保存主按钮缺少真实控件文案")
    save_interior = Rect(save.left + dpi_scale(dpi, 10),
                         save.top + dpi_scale(dpi, 5),
                         save.right - dpi_scale(dpi, 10),
                         save.bottom - dpi_scale(dpi, 5))
    ink = shot.count_ink(save_interior)
    require(ink >= 12,
            f"{label} 保存主按钮未绘制图标/文字：内部前景像素仅 {ink} 个，"
            f"IDC_SAVE={save.as_list()}")
    report["checks"].append(f"{label}: 保存主按钮内部真实前景像素 {ink} 个")
    report["checks"].append(f"{label}: 18 个主要真实控件可见，布局/品牌色/选中态通过")
    report["layouts"].append({"name": label, "client": client.as_list(),
                              "controls": {name: rect.as_list()
                                           for name, rect in rects.items()}})


def check_find_panel(native: Native, window: Win32Window, shot: Screenshot,
                     report: dict) -> None:
    panel = control_rect(native, window, IDC_RESPONSE_FIND_PANEL, "响应查找栏")
    client = native.client_rect_on_screen(window.hwnd)
    inside(client, panel, "响应查找栏")
    parts = [control_rect(native, window, control_id, label)
             for control_id, label in ((IDC_RESPONSE_FIND_EDIT, "查找输入"),
                                       (IDC_FIND_STATUS, "匹配状态"),
                                       (IDC_RESPONSE_FIND_PREV, "上一个"),
                                       (IDC_RESPONSE_FIND_NEXT, "下一个"),
                                       (IDC_RESPONSE_FIND_CLOSE, "关闭查找"))]
    for label, rect in zip(("查找输入", "匹配状态", "上一个", "下一个", "关闭查找"), parts):
        inside(panel, rect, label)
    for left, right, labels in zip(parts, parts[1:],
                                   ("输入/状态", "状态/上一个", "上一个/下一个", "下一个/关闭")):
        separated(left, right, labels)
    # The find bar is white with a distinct input border. This sample is away
    # from icons/text and proves it was actually painted in the captured pixels.
    color_near(shot.pixel(panel.left + 8, panel.top + 6),
               (255, 255, 255), "查找栏背景")
    report["checks"].append("查找栏真实展开后，五个控件可见且不重叠")
    report["layouts"].append({"name": "find_panel", "panel": panel.as_list(),
                              "children": [part.as_list() for part in parts]})


def check_empty_ui(native: Native, window: Win32Window, shot: Screenshot,
                   report: dict) -> None:
    client = native.client_rect_on_screen(window.hwnd)
    title = control_rect(native, window, IDC_EMPTY_TITLE, "空状态标题")
    help_text = control_rect(native, window, IDC_EMPTY_HELP, "空状态提示")
    cta = control_rect(native, window, IDC_EMPTY_NEW_REQUEST, "新建接口主按钮")
    for name, rect in (("空状态标题", title), ("空状态提示", help_text), ("主按钮", cta)):
        inside(client, rect, name)
    above(title, help_text, "空状态标题/提示")
    above(help_text, cta, "空状态提示/主按钮")
    require(not native.user32.IsWindowVisible(window.control(IDC_URL)),
            "无接口时 URL 编辑器仍可见")
    require(window.text(IDC_EMPTY_TITLE) == "还没有打开接口", "空状态标题文案异常")
    dpi = native.user32.GetDpiForWindow(window.hwnd)
    color_near(shot.pixel(cta.left + dpi_scale(dpi, 12),
                          (cta.top + cta.bottom) // 2),
               (37, 99, 235), "空状态主按钮品牌蓝")
    report["checks"].append("空工作区真实画面：标题、提示和主按钮可见，顺序和主色通过")
    report["layouts"].append({"name": "empty", "client": client.as_list(),
                              "title": title.as_list(), "help": help_text.as_list(),
                              "new_request": cta.as_list()})


def click_section_tab(native: Native, window: Win32Window,
                      control_id: int, index: int) -> None:
    """Use real native tab mouse messages (no program state injection)."""
    dpi = native.user32.GetDpiForWindow(window.hwnd)
    x, y = dpi_scale(dpi, 68 * index + 34), dpi_scale(dpi, 15)
    position = (y << 16) | x
    hwnd = window.control(control_id)
    window.send(hwnd, WM_LBUTTONDOWN, 1, position)
    window.send(hwnd, WM_LBUTTONUP, 0, position)
    require(window.send(hwnd, TCM_GETCURSEL) == index,
            f"真实鼠标消息未切换标签 {control_id} 至 {index}")


def check_real_json_body(native: Native, window: Win32Window,
                         shot: Screenshot, report: dict) -> None:
    body = control_rect(native, window, IDC_RESPONSE_BODY, "真实 JSON 响应正文")
    inside(native.client_rect_on_screen(window.hwnd), body, "真实 JSON 响应正文")
    selected_tab_underline(native, window, shot, IDC_RESPONSE_TABS,
                           "真实响应 Body 标签")
    rich_edit = ctypes.create_unicode_buffer(128)
    native.user32.GetClassNameW(window.control(IDC_RESPONSE_BODY), rich_edit,
                                len(rich_edit))
    require(rich_edit.value.upper().startswith("RICHEDIT"),
            f"正式 EXE 未加载富文本响应控件，无法验证语法色: {rich_edit.value!r}")
    key_pixels = shot.count_color(body, (29, 78, 160))
    string_pixels = shot.count_color(body, (21, 128, 61))
    require(key_pixels >= 12 and string_pixels >= 12,
            f"真实 JSON 响应未呈现键/字符串语法色: key={key_pixels}, string={string_pixels}")
    report["checks"].append(f"真实 HTTP JSON 富文本语法色：键 {key_pixels} 像素，字符串 {string_pixels} 像素")


def check_real_headers(native: Native, window: Win32Window,
                       shot: Screenshot, report: dict) -> None:
    headers = control_rect(native, window, IDC_RESPONSE_HEADERS, "真实响应 Headers 列表")
    tabs = control_rect(native, window, IDC_RESPONSE_TABS, "响应标签")
    inside(native.client_rect_on_screen(window.hwnd), headers, "真实响应 Headers 列表")
    above(tabs, headers, "Headers 标签/真实响应头")
    selected_tab_underline(native, window, shot, IDC_RESPONSE_TABS,
                           "真实响应 Headers 标签", 1)
    count = window.send(window.control(IDC_RESPONSE_HEADERS), LVM_GETITEMCOUNT)
    require(0 < count < 10000, f"真实 HTTP 响应头未出现在正式 GUI 列表: {count}")
    report["checks"].append(f"真实 HTTP Headers 标签可见，正式 ListView 显示 {count} 行")


def check_real_search_highlight(native: Native, window: Win32Window,
                                shot: Screenshot, report: dict) -> None:
    check_find_panel(native, window, shot, report)
    status = window.text(IDC_FIND_STATUS)
    require(re.fullmatch(r"\d+ / \d+", status) is not None,
            f"真实 JSON 正文查找状态异常: {status!r}")
    body = control_rect(native, window, IDC_RESPONSE_BODY, "真实 JSON 响应正文")
    highlighted = shot.count_color(body, (147, 197, 253))
    require(highlighted >= 10,
            f"真实正文的查找命中未绘制选中高亮: {highlighted} 像素")
    report["checks"].append(f"真实 JSON 查找命中 {status}，高亮背景 {highlighted} 像素")


def capture_named(native: Native, window: Win32Window, output: Path,
                   name: str, report: dict) -> Screenshot:
    time.sleep(0.2)
    file = output / f"{name}.png"
    before = native.occluders_above(window.hwnd)
    error_title = lambda item: ("FeatherApi.exe" in item["title"] and
                                ("应用程序错误" in item["title"] or
                                 "Application Error" in item["title"]))
    # A dialog from another process can cover the physical screenshot. The
    # production HWND still paints its own content with PrintWindow. A dialog
    # owned by the current process, or a Windows application error dialog for
    # this executable (even if hosted by WerFault), is a real failure.
    foreign_before = any(item["pid"] != window.pid and not error_title(item)
                         for item in before)
    shot = capture_with_timeout(window.hwnd, file, report["capture_timeout_seconds"],
                                force_print=foreign_before)
    after = native.occluders_above(window.hwnd)
    visible_occluders = before + [item for item in after if item not in before]
    if visible_occluders:
        report.setdefault("occlusions", []).append({"screenshot": name,
                                                   "windows": visible_occluders})
    if (not foreign_before and shot.method.startswith("physical desktop") and
            any(item["pid"] != window.pid and not error_title(item)
                for item in after)):
        shot = capture_with_timeout(window.hwnd, file, report["capture_timeout_seconds"],
                                    force_print=True)
    report["screenshots"].append({"path": str(file), "method": shot.method})
    print(f"SCREENSHOT {file} ({shot.method}, {shot.rect.width}x{shot.rect.height}, "
          f"actual DPI={native.user32.GetDpiForWindow(window.hwnd)})", flush=True)
    product_dialogs = [item for item in visible_occluders
                       if item["pid"] == window.pid or error_title(item)]
    require(not product_dialogs,
            "正式 FeatherApi.exe 出现应用错误或异常顶层窗口："
            + json.dumps(product_dialogs, ensure_ascii=False))
    return shot


def use_per_monitor_dpi() -> None:
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    try:
        user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        user32.SetProcessDpiAwarenessContext.restype = wintypes.BOOL
        if user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
            return
    except AttributeError:
        pass
    try:
        shcore = ctypes.WinDLL("shcore", use_last_error=True)
        shcore.SetProcessDpiAwareness.argtypes = [ctypes.c_int]
        shcore.SetProcessDpiAwareness.restype = ctypes.c_long
        if shcore.SetProcessDpiAwareness(2) == 0:
            return
    except (OSError, AttributeError):
        pass
    user32.SetProcessDPIAware.argtypes = []
    user32.SetProcessDPIAware.restype = wintypes.BOOL
    require(bool(user32.SetProcessDPIAware()),
            "测试进程无法启用 DPI 感知，屏幕物理像素坐标不可靠")


def run(args: argparse.Namespace) -> None:
    require(sys.platform == "win32", "视觉测试需要 Windows 交互式桌面")
    require(args.exe.is_file(), f"找不到正式 EXE: {args.exe}")
    require(math.isfinite(args.capture_timeout) and args.capture_timeout > 0,
            "--capture-timeout 必须是大于零的有限秒数")
    require(args.skip_live_response or
            (urlsplit(args.live_url).scheme in ("http", "https") and
             bool(urlsplit(args.live_url).hostname)),
            "--live-url 必须是完整的真实 HTTP/HTTPS 地址")
    use_per_monitor_dpi()
    native = Native()
    monitors = native.monitors()
    output = args.output.resolve() / f"run-{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:8]}"
    output.mkdir(parents=True)
    report = {"exe": str(args.exe.resolve()), "capture_method": "desktop BitBlt, real HWND PrintWindow fallback",
              "capture_timeout_seconds": args.capture_timeout,
              "data_policy": "independent FEATHERAPI_DATA_PATH files in this run directory",
              "screenshots": [], "checks": [], "layouts": [], "monitors": [],
              "test_process_pid": os.getpid(), "status": "running"}
    process = None
    window = None
    try:
        data_path = output / "loaded-data.json"
        seed_data(data_path)
        process, window = launch(args.exe.resolve(), data_path, args.startup_timeout)
        report["loaded_exe_pid"] = process.pid
        require_isolated_workspace(window, data_path)
        print("PASS 生产 EXE 从隔离真实磁盘数据启动", flush=True)
        for index, monitor in enumerate(monitors, 1):
            work = monitor["work"]
            # Use the monitor's actual DPI after moving. The first placement
            # uses the current DPI only as a size estimate; WM_DPICHANGED is
            # emitted by Windows itself if this is a different-DPI monitor.
            dpi = native.user32.GetDpiForWindow(window.hwnd)
            target_width = min(work.width, dpi_scale(dpi, 1280))
            target_height = min(work.height, dpi_scale(dpi, 820))
            if work.width < dpi_scale(dpi, 980) or work.height < dpi_scale(dpi, 640):
                report["monitors"].append({"index": index, "work": work.as_list(),
                                           "status": "skipped: work area below application minimum"})
                continue
            native.place(window.hwnd, work, target_width, target_height)
            actual_dpi = native.user32.GetDpiForWindow(window.hwnd)
            if actual_dpi != dpi:
                target_width = min(work.width, dpi_scale(actual_dpi, 1280))
                target_height = min(work.height, dpi_scale(actual_dpi, 820))
                native.place(window.hwnd, work, target_width, target_height)
            shot = capture_named(native, window, output, f"monitor-{index}-overview", report)
            check_loaded_ui(native, window, shot, report, f"monitor-{index}-overview")
            report["monitors"].append({"index": index, "primary": monitor["primary"],
                                       "work": work.as_list(),
                                       "actual_get_dpi_for_window": native.user32.GetDpiForWindow(window.hwnd),
                                       "status": "passed"})
            print(f"PASS 显示器 {index} 实际 DPI {actual_dpi}：主界面布局及像素", flush=True)
            if index == 1:
                min_width = dpi_scale(actual_dpi, 980)
                min_height = dpi_scale(actual_dpi, 640)
                native.place(window.hwnd, work, min_width, min_height)
                shot = capture_named(native, window, output, "minimum-window", report)
                check_loaded_ui(native, window, shot, report, "minimum-window")
                print("PASS 最小窗口尺寸布局及像素", flush=True)
                window.click(IDC_RESPONSE_FIND)
                shot = capture_named(native, window, output, "find-panel", report)
                check_find_panel(native, window, shot, report)
                window.click(IDC_RESPONSE_FIND_CLOSE)
                print("PASS 真实响应查找栏展开后的布局及像素", flush=True)
        require(any(m["status"] == "passed" for m in report["monitors"]),
                "没有工作区足以容纳 FeatherApi 最小窗口的显示器")

        if args.skip_live_response:
            report["checks"].append("SKIP 真实 HTTP 响应样式（显式 --skip-live-response）")
        else:
            primary = monitors[0]["work"]
            dpi = native.user32.GetDpiForWindow(window.hwnd)
            native.place(window.hwnd, primary,
                         min(primary.width, dpi_scale(dpi, 1280)),
                         min(primary.height, dpi_scale(dpi, 820)))
            window.set_text(IDC_URL, args.live_url)
            window.click(IDC_SEND)
            body = wait_http(window, process, 200, args.expected_body_fragment,
                             args.request_timeout)
            require(bool(body.strip()), "真实服务返回空响应，无法验证产品响应样式")
            shot = capture_named(native, window, output, "live-json-body", report)
            check_real_json_body(native, window, shot, report)
            print("PASS 真实 HTTP JSON 响应富文本语法色", flush=True)

            click_section_tab(native, window, IDC_RESPONSE_TABS, 1)
            shot = capture_named(native, window, output, "live-response-headers", report)
            check_real_headers(native, window, shot, report)
            print("PASS 真实 HTTP Headers 标签及响应头列表", flush=True)

            click_section_tab(native, window, IDC_RESPONSE_TABS, 0)
            window.click(IDC_RESPONSE_FIND)
            window.set_text(IDC_RESPONSE_FIND_EDIT, args.search_term)
            shot = capture_named(native, window, output, "live-search-highlight", report)
            check_real_search_highlight(native, window, shot, report)
            print("PASS 真实 HTTP 响应正文查找高亮", flush=True)
            window.click(IDC_RESPONSE_FIND_CLOSE)
            # The URL was edited through the real GUI. Commit it to this run's
            # isolated file so WM_CLOSE does not wait on an unsaved-changes dialog.
            window.click(IDC_SAVE)

        stop(process, window, args.shutdown_timeout)
        process = None

        empty_path = output / "empty-data.json"
        empty_path.write_text(json.dumps({"Settings": {"SidebarWidth": 320,
                                                        "RequestPanelHeight": 330},
                                          "Folders": []}), encoding="utf-8")
        process, window = launch(args.exe.resolve(), empty_path, args.startup_timeout)
        report["empty_exe_pid"] = process.pid
        primary = monitors[0]["work"]
        dpi = native.user32.GetDpiForWindow(window.hwnd)
        native.place(window.hwnd, primary, min(primary.width, dpi_scale(dpi, 1280)),
                     min(primary.height, dpi_scale(dpi, 820)))
        shot = capture_named(native, window, output, "empty-state", report)
        check_empty_ui(native, window, shot, report)
        print("PASS 空工作区真实画面布局及像素", flush=True)
        stop(process, window, args.shutdown_timeout)
        process = None
        report["status"] = "passed"
    except BaseException as error:
        report["status"] = "failed"
        report["failure"] = str(error)
        raise
    finally:
        force_stop(process)
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False,
                                                       indent=2), encoding="utf-8")
        print(f"VISUAL REPORT {output / 'report.json'}", flush=True)


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--capture-worker":
        return capture_worker(sys.argv[2:])
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=project / "build/release/FeatherApi.exe")
    parser.add_argument("--output", type=Path, default=project / "build/real-visual")
    parser.add_argument("--startup-timeout", type=float, default=15)
    parser.add_argument("--shutdown-timeout", type=float, default=10)
    parser.add_argument("--capture-timeout", type=float, default=15,
                        help="每次真实窗口截图的超时秒数，超时后终止辅助进程并报错")
    parser.add_argument("--live-url", default="https://httpbin.org/get",
                        help="真实 JSON 服务，默认 https://httpbin.org/get")
    parser.add_argument("--expected-body-fragment", default='"url"',
                        help="真实响应正文必须包含的片段")
    parser.add_argument("--search-term", default="url",
                        help="在真实 JSON 响应正文中查找的单词")
    parser.add_argument("--request-timeout", type=float, default=45)
    parser.add_argument("--skip-live-response", action="store_true",
                        help="仅供离线排查；跳过真实 HTTP 内容样式截图")
    args = parser.parse_args()
    try:
        run(args)
    except (E2EFailure, OSError) as error:
        print(f"FAIL {error}", flush=True)
        return 1
    print("PASS 正式 EXE 视觉自动化检查完成", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
