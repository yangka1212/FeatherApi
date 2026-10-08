#!/usr/bin/env python3
"""Exercise the built FeatherApi.exe, a public API, and a real isolated data file.

No application functions are imported or replaced. The script operates the production
Win32 window and reads the file written by the production storage implementation.
Run on an interactive Windows desktop after building the Release executable.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from urllib.parse import parse_qsl, urlsplit
import uuid

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")


DEFAULT_URL = "https://api.github.com/repos/yangka1212/FeatherApi"
DEFAULT_FRAGMENT = '"full_name":"yangka1212/FeatherApi"'
DEFAULT_ERROR_URL = (
    "https://api.github.com/repos/yangka1212/FeatherApi/"
    "featherapi-e2e-missing-resource"
)
SEED_URL = "https://example.invalid/featherapi-e2e-seed"
FOLDER_ID = "featherapi-real-e2e-folder"
REQUEST_ID = "featherapi-real-e2e-request"

WM_CLOSE = 0x0010
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E
BM_CLICK = 0x00F5
SMTO_ABORTIFHUNG = 0x0002

# Source of truth: the sequential IDC enum in src/ui/main_window.cpp.
IDC_URL = 105
IDC_SAVE = 106
IDC_SEND = 108
IDC_VALIDATION = 118
IDC_SUMMARY = 119
IDC_RESPONSE_BODY = 121
IDC_RESPONSE_FIND_PANEL = 123
IDC_RESPONSE_FIND_EDIT = 124
IDC_RESPONSE_FIND_CLOSE = 127
IDC_RESPONSE_FIND = 134
IDC_FIND_STATUS = 135


class E2EFailure(RuntimeError):
    pass


def report(message: str) -> None:
    print(message, flush=True)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise E2EFailure(message)


def response_contains(body: str, fragment: str) -> bool:
    if fragment in body:
        return True
    try:
        compact = json.dumps(json.loads(body), ensure_ascii=False, separators=(",", ":"))
    except (ValueError, TypeError):
        return False
    return fragment in compact


def seed_data(path: Path) -> None:
    # This is a real on-disk workspace, using the public JSON storage format.
    data = {
        "Settings": {"SidebarWidth": 320, "RequestPanelHeight": 330},
        "Folders": [{
            "Id": FOLDER_ID,
            "Name": "真实网络端到端测试",
            "IsExpanded": True,
            "Children": [],
            "Requests": [{
                "Id": REQUEST_ID,
                "Name": "真实公共 API",
                "Method": "GET",
                "Url": SEED_URL,
                "QueryParams": [],
                "Headers": [
                    {"IsEnabled": True, "Key": "User-Agent", "Value": "FeatherApi-RealE2E",
                     "Type": "string", "Description": ""},
                    {"IsEnabled": True, "Key": "Accept", "Value": "application/json",
                     "Type": "string", "Description": ""},
                ],
                "BodyType": "None",
                "BodyContent": "",
                "FormFields": [],
                "Cases": [],
            }],
        }],
    }
    path.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")


@contextmanager
def isolated_workspace():
    # tempfile.TemporaryDirectory creates a private Windows ACL that is not
    # writable by some sandboxed desktop sessions. A UUID directory under the
    # ignored build tree inherits the workspace ACL and is equally isolated.
    root = (Path(__file__).resolve().parents[1] / "build" / "real-e2e").resolve()
    root.mkdir(parents=True, exist_ok=True)
    run_dir = root / f"run-{uuid.uuid4().hex}"
    run_dir.mkdir()
    try:
        yield run_dir
    finally:
        # Verify the resolved target before any recursive cleanup on Windows.
        if run_dir.exists() and run_dir.resolve().parent == root:
            shutil.rmtree(run_dir)


def find_seeded_request(data_path: Path) -> dict:
    try:
        data = json.loads(data_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise E2EFailure(f"无法读取生产程序写出的隔离数据文件: {error}") from error
    for folder in data.get("Folders", []):
        for request in folder.get("Requests", []):
            if request.get("Id") == REQUEST_ID:
                return request
    raise E2EFailure("隔离数据文件中找不到预置接口，生产程序可能未使用 FEATHERAPI_DATA_PATH")


def persisted_url(request: dict) -> str:
    # The UI moves a URL's query into QueryParams before saving.
    address = request.get("Url", "")
    entries = [entry for entry in request.get("QueryParams", [])
               if entry.get("IsEnabled", entry.get("Enabled", True)) and entry.get("Key", "").strip()]
    if not entries:
        return address
    from urllib.parse import quote
    fragment = ""
    if "#" in address:
        address, fragment = address.split("#", 1)
        fragment = "#" + fragment
    separator = "?" if "?" not in address else ("" if address.endswith(("?", "&")) else "&")
    query = "&".join(
        f"{quote(str(entry['Key']).strip(), safe='-_.~')}={quote(str(entry.get('Value', '')), safe='-_.~')}"
        for entry in entries
    )
    return address + separator + query + fragment


def urls_equivalent(left: str, right: str) -> bool:
    a, b = urlsplit(left), urlsplit(right)
    return ((a.scheme, a.netloc, a.path, a.fragment) ==
            (b.scheme, b.netloc, b.path, b.fragment) and
            parse_qsl(a.query, keep_blank_values=True) ==
            parse_qsl(b.query, keep_blank_values=True))


class Win32Window:
    def __init__(self, pid: int):
        self.pid = pid
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self._configure_api()
        self.hwnd = 0

    def _configure_api(self) -> None:
        u = self.user32
        u.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
        u.EnumWindows.restype = wintypes.BOOL
        u.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        u.GetClassNameW.restype = ctypes.c_int
        u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        u.GetWindowThreadProcessId.restype = wintypes.DWORD
        u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
        u.GetDlgItem.restype = wintypes.HWND
        u.IsWindow.argtypes = [wintypes.HWND]
        u.IsWindow.restype = wintypes.BOOL
        u.IsWindowVisible.argtypes = [wintypes.HWND]
        u.IsWindowVisible.restype = wintypes.BOOL
        u.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                                          wintypes.LPARAM, wintypes.UINT, wintypes.UINT,
                                          ctypes.POINTER(ctypes.c_size_t)]
        u.SendMessageTimeoutW.restype = ctypes.c_ssize_t
        u.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        u.PostMessageW.restype = wintypes.BOOL

    def locate(self) -> int:
        found = []
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def inspect(hwnd: int, _unused: int) -> bool:
            process_id = wintypes.DWORD()
            self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(process_id))
            if process_id.value == self.pid:
                name = ctypes.create_unicode_buffer(128)
                self.user32.GetClassNameW(hwnd, name, len(name))
                if name.value == "FeatherApiWindow":
                    found.append(hwnd)
            return True

        callback = callback_type(inspect)
        self.user32.EnumWindows(callback, 0)
        self.hwnd = found[0] if found else 0
        return self.hwnd

    def control(self, control_id: int) -> int:
        require(self.hwnd and self.user32.IsWindow(self.hwnd), "主窗口已退出")
        parent = self.hwnd
        if control_id in (IDC_RESPONSE_FIND_EDIT, IDC_RESPONSE_FIND_CLOSE, IDC_FIND_STATUS):
            parent = self.user32.GetDlgItem(self.hwnd, IDC_RESPONSE_FIND_PANEL)
        hwnd = self.user32.GetDlgItem(parent, control_id)
        require(bool(hwnd), f"找不到 Win32 控件 ID {control_id}，EXE 与脚本版本可能不匹配")
        return hwnd

    def send(self, hwnd: int, message: int, wparam: int = 0, lparam: int = 0) -> int:
        result = ctypes.c_size_t()
        ctypes.set_last_error(0)
        ok = self.user32.SendMessageTimeoutW(hwnd, message, wparam, lparam,
                                             SMTO_ABORTIFHUNG, 5000, ctypes.byref(result))
        if not ok:
            error = ctypes.get_last_error()
            raise E2EFailure(f"Win32 消息 0x{message:04x} 发送失败或窗口无响应 (GetLastError={error})")
        return result.value

    def text(self, control_id: int) -> str:
        hwnd = self.control(control_id)
        length = min(self.send(hwnd, WM_GETTEXTLENGTH), 6 * 1024 * 1024)
        buffer = ctypes.create_unicode_buffer(length + 1)
        self.send(hwnd, WM_GETTEXT, len(buffer), ctypes.addressof(buffer))
        return buffer.value

    def set_text(self, control_id: int, value: str) -> None:
        buffer = ctypes.create_unicode_buffer(value)
        require(self.send(self.control(control_id), WM_SETTEXT, 0, ctypes.addressof(buffer)) != 0,
                f"无法写入 Win32 控件 ID {control_id}")

    def click(self, control_id: int) -> None:
        self.send(self.control(control_id), BM_CLICK)

    def close(self) -> None:
        if self.hwnd and self.user32.IsWindow(self.hwnd):
            if not self.user32.PostMessageW(self.hwnd, WM_CLOSE, 0, 0):
                raise E2EFailure(f"无法关闭测试窗口 (GetLastError={ctypes.get_last_error()})")


def until(description: str, timeout: float, test, process: subprocess.Popen) -> object:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if process.poll() is not None:
            raise E2EFailure(f"等待{description}时生产程序提前退出 (exit={process.returncode})")
        result = test()
        if result:
            return result
        time.sleep(0.1)
    raise E2EFailure(f"等待{description}超时 ({timeout:g} 秒)")


def launch(exe: Path, data_path: Path, startup_timeout: float) -> tuple[subprocess.Popen, Win32Window]:
    env = os.environ.copy()
    env["FEATHERAPI_DATA_PATH"] = str(data_path)
    process = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env)
    window = Win32Window(process.pid)
    try:
        until("可见的 FeatherApi 主窗口", startup_timeout,
              lambda: window.locate() and window.user32.IsWindowVisible(window.hwnd), process)
        return process, window
    except BaseException:
        force_stop(process)
        raise


def require_isolated_workspace(window: Win32Window, data_path: Path) -> None:
    expected = persisted_url(find_seeded_request(data_path))
    actual = window.text(IDC_URL)
    require(actual == expected,
            "启动后窗口未显示隔离数据中的预置接口；为保护用户数据，测试已停止。"
            f" UI URL={actual!r}, 隔离数据 URL={expected!r}")


def check_response_find(window: Win32Window, body: str) -> None:
    terms = re.findall(r"[A-Za-z_][A-Za-z0-9_/-]{4,}", body)
    require(bool(terms), "真实响应没有可用于查找功能验证的单词")
    term = terms[0]
    window.click(IDC_RESPONSE_FIND)
    window.set_text(IDC_RESPONSE_FIND_EDIT, term)
    status = window.text(IDC_FIND_STATUS)
    require(re.match(r"^\d+ / \d+$", status) is not None,
            f"响应查找没有定位真实正文中的 {term!r}: {status!r}")
    window.click(IDC_RESPONSE_FIND_CLOSE)


def wait_http(window: Win32Window, process: subprocess.Popen, status: int,
              fragment: str, timeout: float) -> str:
    def completed():
        summary = window.text(IDC_SUMMARY)
        if summary.startswith("网络错误") or summary.startswith("已取消"):
            detail = window.text(IDC_VALIDATION)
            raise E2EFailure(f"真实网络请求失败: {summary}; {detail}. 请检查网络、代理及 TLS 证书。")
        match = re.match(r"^(\d{3})\s", summary)
        return (int(match.group(1)), summary) if match else None

    actual, summary = until(f"HTTP {status} 响应", timeout, completed, process)
    require(actual == status, f"真实服务返回 HTTP {actual}，期望 {status}: {summary}; "
            f"响应开头={window.text(IDC_RESPONSE_BODY)[:300]!r}")
    body = window.text(IDC_RESPONSE_BODY)
    require(response_contains(body, fragment),
            f"HTTP {status} 响应缺少 {fragment!r}: 响应开头={body[:300]!r}")
    return body


def stop(process: subprocess.Popen, window: Win32Window, timeout: float) -> None:
    exit_code = process.poll()
    require(exit_code is None,
            f"生产程序在请求关闭前已提前退出 (exit={exit_code})")
    window.close()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise E2EFailure(f"生产程序在 WM_CLOSE 后 {timeout:g} 秒内未退出") from error
    require(process.returncode == 0, f"生产程序关闭时返回非零退出码 {process.returncode}")


def force_stop(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def run(args: argparse.Namespace) -> None:
    require(sys.platform == "win32", "此端到端测试需要 Windows 与交互式桌面")
    exe = args.exe.resolve()
    require(exe.is_file(), f"找不到生产 EXE: {exe}；请先运行 scripts/build.ps1 -Action Test")
    require(urlsplit(args.url).scheme in ("http", "https") and urlsplit(args.url).hostname,
            "--url 必须是完整的 HTTP/HTTPS 地址")
    require(bool(args.expected_body_fragment), "--expected-body-fragment 不能为空")
    if args.error_url:
        require(urlsplit(args.error_url).scheme in ("http", "https") and urlsplit(args.error_url).hostname,
                "--error-url 必须是完整的 HTTP/HTTPS 地址")

    with isolated_workspace() as temporary:
        data_path = temporary / "data.json"
        seed_data(data_path)
        process = None
        window = None
        try:
            process, window = launch(exe, data_path, args.startup_timeout)
            require_isolated_workspace(window, data_path)
            require(window.text(IDC_URL) == SEED_URL, "首次启动未加载预置的独立测试数据")
            report("PASS 生产 EXE 从独立真实数据文件启动")

            window.set_text(IDC_URL, args.url)
            require(window.text(IDC_URL) == args.url, "UI 未保留通过真实控件输入的 URL")
            window.click(IDC_SEND)
            good_body = wait_http(window, process, args.expected_status,
                                  args.expected_body_fragment, args.request_timeout)
            report(f"PASS 真实公共 API 返回 HTTP {args.expected_status}，GUI 显示预期正文")
            check_response_find(window, good_body)
            report("PASS GUI 在真实响应正文中查找并定位文字")

            previous_summary = window.text(IDC_SUMMARY)
            window.set_text(IDC_URL, "not-a-url")
            window.click(IDC_SEND)
            validation = window.text(IDC_VALIDATION)
            require("HTTP" in validation and "HTTPS" in validation,
                    f"无效 URL 未显示校验信息: {validation!r}")
            require(window.text(IDC_RESPONSE_BODY) == good_body,
                    "无效 URL 覆盖了上一次真实响应")
            require(window.text(IDC_SUMMARY) == previous_summary,
                    "无效 URL 触发了请求状态变化")
            report("PASS 无效 URL 被实际 GUI 拒绝，已有响应保留")

            window.set_text(IDC_URL, args.url)
            window.click(IDC_SAVE)
            saved = find_seeded_request(data_path)
            require(saved.get("Method") == "GET" and urls_equivalent(persisted_url(saved), args.url),
                    f"真实文件保存的请求与 UI 不一致: {persisted_url(saved)!r}")
            report("PASS 生产存储将 GUI 编辑保存到独立真实数据文件")
            stop(process, window, args.shutdown_timeout)
            process = None
            report("PASS 生产程序正常关闭")

            process, window = launch(exe, data_path, args.startup_timeout)
            require_isolated_workspace(window, data_path)
            require(urls_equivalent(window.text(IDC_URL), args.url),
                    f"重启后生产 GUI 未恢复保存的 URL: {window.text(IDC_URL)!r}")
            report("PASS 关闭并重启后，生产 GUI 恢复真实磁盘数据")

            if args.error_url:
                window.set_text(IDC_URL, args.error_url)
                window.click(IDC_SEND)
                wait_http(window, process, args.error_status,
                          args.error_body_fragment, args.request_timeout)
                report(f"PASS 真实服务的 HTTP {args.error_status} 响应被 GUI 正常显示")
                window.set_text(IDC_URL, args.url)
                window.click(IDC_SAVE)
                require(urls_equivalent(persisted_url(find_seeded_request(data_path)), args.url),
                        "非 2xx 场景后未恢复隔离数据文件中的主请求 URL")
            else:
                report("SKIP 外部非 2xx 请求 (--error-url 未设置)")

            stop(process, window, args.shutdown_timeout)
            process = None
            corrupt_path = temporary / "corrupt.json"
            corrupt_bytes = b"{bad json from real file"
            corrupt_path.write_bytes(corrupt_bytes)
            process, window = launch(exe, corrupt_path, args.startup_timeout)
            require(window.text(IDC_URL) == "", "损坏文件启动时未进入空工作区")
            backups = list(temporary.glob("corrupt.json.corrupt-*"))
            require(len(backups) == 1 and backups[0].read_bytes() == corrupt_bytes,
                    "生产存储未备份损坏的 JSON 数据文件")
            report("PASS 损坏的真实 JSON 文件触发原样备份并启动空工作区")
            stop(process, window, args.shutdown_timeout)
            process = None
            report("PASS 所有真实端到端场景完成")
        finally:
            force_stop(process)


def main() -> int:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=project_root / "build/release/FeatherApi.exe",
                        help="生产 FeatherApi.exe 路径")
    parser.add_argument("--url", default=DEFAULT_URL, help="真实 HTTP/HTTPS GET 接口 URL")
    parser.add_argument("--expected-status", type=int, default=200,
                        help="主请求期望状态码，默认 200")
    parser.add_argument("--expected-body-fragment", default=DEFAULT_FRAGMENT,
                        help="主请求响应正文必须包含的片段；JSON 会同时检查紧凑格式")
    parser.add_argument("--error-url", default=DEFAULT_ERROR_URL,
                        help="返回非 2xx 的真实接口 URL")
    parser.add_argument("--skip-error", action="store_true", help="跳过真实非 2xx 请求")
    parser.add_argument("--error-status", type=int, default=404,
                        help="错误请求期望状态码，默认 404")
    parser.add_argument("--error-body-fragment", default='"message":"Not Found"',
                        help="错误请求响应正文必须包含的片段")
    parser.add_argument("--startup-timeout", type=float, default=15,
                        help="窗口启动超时秒数")
    parser.add_argument("--request-timeout", type=float, default=45,
                        help="单个真实网络请求超时秒数")
    parser.add_argument("--shutdown-timeout", type=float, default=10,
                        help="程序正常关闭超时秒数")
    args = parser.parse_args()
    if args.skip_error:
        args.error_url = ""
    try:
        run(args)
    except (E2EFailure, OSError) as error:
        report(f"FAIL {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
