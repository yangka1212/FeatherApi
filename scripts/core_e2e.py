#!/usr/bin/env python3
"""Exercise catalog and editor workflows in the production FeatherApi.exe.

This drives the actual Win32 window, prompt dialogs, tree, tabs and editors. It
never imports application code, replaces a service, or uses the user's data file.
The optional OpenAPI case downloads a real document through the production app.
Run from an interactive Windows desktop after building the Release executable.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import struct
import sys
import time
from urllib.parse import urlsplit

from real_e2e import (E2EFailure, Win32Window, force_stop, isolated_workspace,
                      launch, report, require, stop, until, wait_http)


SEED_FOLDER_ID = "core-e2e-source-folder"
TARGET_FOLDER_ID = "core-e2e-target-folder"
SEED_REQUEST_ID = "core-e2e-seed-request"
SEED_URL = "https://example.invalid/core-e2e-seed"
NEW_FOLDER = "核心功能子目录"
RENAMED_FOLDER = "核心功能目录已重命名"
NEW_REQUEST = "核心功能接口"
RENAMED_REQUEST = "核心功能接口已重命名"
CASE_NAME = "核心功能用例"
REQUEST_PATH_QUERY = "/anything/core-e2e?case=real&mode=post"
BODY = '{"source":"production GUI","count":2}'

WM_COMMAND = 0x0111
WM_NULL = 0x0000
WM_CONTEXTMENU = 0x007B
WM_SYSCOMMAND = 0x0112
WM_KEYDOWN = 0x0100
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_APP = 0x8000
WM_EDIT_ENTRY_CELL = WM_APP + 4
WM_SETTEXT = 0x000C
BM_CLICK = 0x00F5
TVM_GETNEXTITEM = 0x110A
TVM_SELECTITEM = 0x110B
TVM_EXPAND = 0x1102
TVE_EXPAND = 2
TVGN_ROOT = 0
TVGN_NEXT = 1
TVGN_CHILD = 4
TVGN_CARET = 9
TCM_GETCURSEL = 0x130B
TCM_GETITEMCOUNT = 0x1304
CB_GETCOUNT = 0x0146
CB_SETCURSEL = 0x014E
CBN_SELCHANGE = 1
VK_DOWN = 0x28
VK_RETURN = 0x0D
GW_CHILD = 5
GW_HWNDNEXT = 2
SC_MINIMIZE = 0xF020
SC_RESTORE = 0xF120
IDOK = 1
IDYES = 6
AF_INET = 2
AF_INET6 = 23
TCP_TABLE_OWNER_PID_ALL = 5
MIB_TCP_STATE_ESTAB = 5
ERROR_INSUFFICIENT_BUFFER = 122

IDC_TREE = 102
IDC_METHOD = 104
IDC_URL = 105
IDC_SAVE = 106
IDC_SEND = 108
IDC_CANCEL = 109
IDC_REQUEST_TABS = 103
IDC_EDITOR_TABS = 110
IDC_BODY_TYPE = 114
IDC_BODY = 117
IDC_SUMMARY = 119
IDC_RESPONSE_BODY = 121
IDC_PROMPT_EDIT = 900
SMALL_DIALOG_COMBO = 911

IDM_FOLDER_ADD_REQUEST = 1000
IDM_FOLDER_ADD_CHILD = 1001
IDM_FOLDER_IMPORT = 1002
IDM_FOLDER_RENAME = 1003
IDM_FOLDER_DELETE = 1004
IDM_REQUEST_RENAME = 1006
IDM_REQUEST_DUPLICATE = 1007
IDM_REQUEST_MOVE = 1008
IDM_REQUEST_DELETE = 1009
IDM_SAVE_CASE = 1010
IDM_CASE_DELETE = 1011


def seed_data(path: Path) -> None:
    """Create a private real JSON workspace with known tree positions."""
    path.write_text(json.dumps({
        "Settings": {"SidebarWidth": 320, "RequestPanelHeight": 330},
        "Folders": [
            {"Id": SEED_FOLDER_ID, "Name": "来源目录", "IsExpanded": True,
             "Children": [], "Requests": [{
                 "Id": SEED_REQUEST_ID, "Name": "隔离数据标记", "Method": "GET",
                 "Url": SEED_URL, "QueryParams": [], "Headers": [],
                 "BodyType": "None", "BodyContent": "", "FormFields": [], "Cases": [],
             }]},
            {"Id": TARGET_FOLDER_ID, "Name": "目标目录", "IsExpanded": True,
             "Children": [], "Requests": []},
        ],
    }, ensure_ascii=False), encoding="utf-8")


def read_data(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise E2EFailure(f"无法读取生产程序写出的独立数据文件: {error}") from error


def source_folder(data: dict) -> dict:
    matches = [folder for folder in data["Folders"] if folder["Id"] == SEED_FOLDER_ID]
    require(len(matches) == 1, "来源目录在隔离数据文件中缺失或重复")
    return matches[0]


def target_folder(data: dict) -> dict:
    matches = [folder for folder in data["Folders"] if folder["Id"] == TARGET_FOLDER_ID]
    require(len(matches) == 1, "目标目录在隔离数据文件中缺失或重复")
    return matches[0]


def configure_more_win32(window: Win32Window) -> None:
    u = window.user32
    u.GetWindow.argtypes = [wintypes.HWND, ctypes.c_uint]
    u.GetWindow.restype = wintypes.HWND
    u.GetDlgCtrlID.argtypes = [wintypes.HWND]
    u.GetDlgCtrlID.restype = ctypes.c_int
    u.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    u.GetWindowTextW.restype = ctypes.c_int
    u.IsWindowEnabled.argtypes = [wintypes.HWND]
    u.IsWindowEnabled.restype = wintypes.BOOL
    u.IsIconic.argtypes = [wintypes.HWND]
    u.IsIconic.restype = wintypes.BOOL
    u.GetDpiForWindow.argtypes = [wintypes.HWND]
    u.GetDpiForWindow.restype = ctypes.c_uint
    u.SetForegroundWindow.argtypes = [wintypes.HWND]
    u.SetForegroundWindow.restype = wintypes.BOOL


def tcp_rows(family: int) -> list[tuple[int, int]]:
    """Read real OS TCP owner/PID rows; no application process is modified."""
    api = ctypes.WinDLL("iphlpapi", use_last_error=True).GetExtendedTcpTable
    api.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD),
                    wintypes.BOOL, wintypes.ULONG, ctypes.c_int, wintypes.ULONG]
    api.restype = wintypes.DWORD
    size = wintypes.DWORD(0)
    status = api(None, ctypes.byref(size), False, family,
                 TCP_TABLE_OWNER_PID_ALL, 0)
    if status not in (0, ERROR_INSUFFICIENT_BUFFER):
        raise E2EFailure(f"无法读取真实 TCP 表 (family={family}, error={status})")
    for _ in range(6):
        buffer = ctypes.create_string_buffer(max(size.value, 4))
        status = api(buffer, ctypes.byref(size), False, family,
                     TCP_TABLE_OWNER_PID_ALL, 0)
        if status == ERROR_INSUFFICIENT_BUFFER:
            continue
        if status != 0:
            raise E2EFailure(f"无法读取真实 TCP 表 (family={family}, error={status})")
        data = buffer.raw[:size.value]
        require(len(data) >= 4, f"真实 TCP 表截断 (family={family})")
        count = struct.unpack_from("<I", data, 0)[0]
        row_size = 24 if family == AF_INET else 56
        state_offset, pid_offset = (0, 20) if family == AF_INET else (48, 52)
        require(4 + count * row_size <= len(data),
                f"真实 TCP 表行数与长度不一致 (family={family})")
        return [(struct.unpack_from("<I", data, 4 + i * row_size + state_offset)[0],
                 struct.unpack_from("<I", data, 4 + i * row_size + pid_offset)[0])
                for i in range(count)]
    raise E2EFailure(f"真实 TCP 表持续变化，无法确认传输状态 (family={family})")


def has_established_tcp(pid: int) -> bool:
    return any(state == MIB_TCP_STATE_ESTAB and owner == pid
               for family in (AF_INET, AF_INET6)
               for state, owner in tcp_rows(family))


def require_real_transport(window: Win32Window, process, context: str) -> None:
    require(process.poll() is None, f"{context}前正式 EXE 已退出")
    summary = window.text(IDC_SUMMARY)
    require(not summary[:3].isdigit() and not summary.startswith(
        ("网络错误", "已取消", "正在取消")),
        f"{context}前真实请求已结束: {summary!r}")
    require(summary.startswith("请求中"),
            f"{context}前请求状态并非发送中: {summary!r}")
    require(bool(window.user32.IsWindowVisible(window.control(IDC_CANCEL))),
            f"{context}前取消按钮已隐藏，无法证明请求仍在飞行")
    require(has_established_tcp(process.pid),
            f"{context}前未发现正式 EXE PID={process.pid} 的 ESTABLISHED TCP 连接")


def wait_real_transport(window: Win32Window, process, context: str,
                        timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        require(process.poll() is None, f"等待{context}期间正式 EXE 已退出")
        summary = window.text(IDC_SUMMARY)
        require(not summary[:3].isdigit() and not summary.startswith(
            ("网络错误", "已取消", "正在取消")),
            f"等待{context}时真实请求已结束: {summary!r}")
        if (summary.startswith("请求中")
                and window.user32.IsWindowVisible(window.control(IDC_CANCEL))
                and has_established_tcp(process.pid)):
            return
        time.sleep(0.05)
    raise E2EFailure(
        f"{context}超时：无法同时确认取消按钮可见和正式 EXE PID={process.pid} "
        "存在 ESTABLISHED TCP 连接")


def class_name(window: Win32Window, hwnd: int) -> str:
    buffer = ctypes.create_unicode_buffer(128)
    window.user32.GetClassNameW(hwnd, buffer, len(buffer))
    return buffer.value


def title(window: Win32Window, hwnd: int) -> str:
    buffer = ctypes.create_unicode_buffer(256)
    window.user32.GetWindowTextW(hwnd, buffer, len(buffer))
    return buffer.value


def top_level(window: Win32Window, wanted_class: str, wanted_title: str = "") -> int:
    found: list[int] = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def inspect(hwnd: int, _unused: int) -> bool:
        pid = wintypes.DWORD()
        window.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if (pid.value == window.pid and window.user32.IsWindowVisible(hwnd)
                and class_name(window, hwnd) == wanted_class
                and (not wanted_title or title(window, hwnd) == wanted_title)):
            found.append(hwnd)
        return True

    callback = callback_type(inspect)
    window.user32.EnumWindows(callback, 0)
    return found[0] if found else 0


def child_descriptions(window: Win32Window, hwnd: int) -> list[tuple[int, str, str]]:
    result = []
    child = window.user32.GetWindow(hwnd, GW_CHILD)
    while child:
        result.append((window.user32.GetDlgCtrlID(child),
                       class_name(window, child), title(window, child)[:80]))
        child = window.user32.GetWindow(child, GW_HWNDNEXT)
    return result


def child_button(window: Win32Window, hwnd: int, label: str) -> int:
    child = window.user32.GetWindow(hwnd, GW_CHILD)
    while child:
        if class_name(window, child) == "Button" and title(window, child) == label:
            return child
        child = window.user32.GetWindow(child, GW_HWNDNEXT)
    return 0


def post(window: Win32Window, hwnd: int, message: int,
         wparam: int = 0, lparam: int = 0) -> None:
    require(bool(window.user32.PostMessageW(hwnd, message, wparam, lparam)),
            f"无法向生产窗口发送事件 0x{message:04x}")


def command(window: Win32Window, command_id: int) -> None:
    # This is the same WM_COMMAND that the live context menu sends to the
    # production window. Modal commands must be posted, so we can fill dialogs.
    post(window, window.hwnd, WM_COMMAND, command_id)


def set_hwnd_text(window: Win32Window, hwnd: int, value: str) -> None:
    buffer = ctypes.create_unicode_buffer(value)
    require(window.send(hwnd, WM_SETTEXT, 0, ctypes.addressof(buffer)) != 0,
            "真实输入控件不接受文字")


def prompt(window: Win32Window, process, command_id: int, value: str,
           expected_title: str, timeout: float) -> None:
    command(window, command_id)
    dialog = until(expected_title, timeout,
                   lambda: top_level(window, "FeatherApiPrompt", expected_title), process)
    edit = window.user32.GetDlgItem(dialog, IDC_PROMPT_EDIT)
    require(bool(edit), f"{expected_title} 对话框缺少输入框")
    set_hwnd_text(window, edit, value)
    accept = window.user32.GetDlgItem(dialog, IDOK)
    require(bool(accept), f"{expected_title} 对话框缺少确定按钮")
    window.send(accept, BM_CLICK)
    until(f"{expected_title} 对话框关闭", timeout,
          lambda: not window.user32.IsWindow(dialog), process)
    until(f"{expected_title} 后主窗口恢复", timeout,
          lambda: window.user32.IsWindowEnabled(window.hwnd), process)
    window.send(window.hwnd, WM_NULL)


def confirm(window: Win32Window, process, command_id: int,
            expected_title: str, timeout: float) -> None:
    command(window, command_id)
    dialog = until(expected_title, timeout,
                   lambda: top_level(window, "#32770", expected_title), process)
    yes = window.user32.GetDlgItem(dialog, IDYES)
    require(bool(yes), f"{expected_title} 对话框缺少是按钮")
    window.send(yes, BM_CLICK)
    until(f"{expected_title} 对话框关闭", timeout,
          lambda: not window.user32.IsWindow(dialog), process)
    until(f"{expected_title} 后主窗口恢复", timeout,
          lambda: window.user32.IsWindowEnabled(window.hwnd), process)
    window.send(window.hwnd, WM_NULL)


def tree_item(window: Win32Window, path: tuple[int, ...]) -> int:
    tree = window.control(IDC_TREE)
    item = 0
    for depth, index in enumerate(path):
        item = window.send(tree, TVM_GETNEXTITEM,
                           TVGN_ROOT if depth == 0 else TVGN_CHILD, item)
        for _ in range(index):
            require(bool(item), f"目录树路径 {path} 不存在")
            item = window.send(tree, TVM_GETNEXTITEM, TVGN_NEXT, item)
        require(bool(item), f"目录树路径 {path} 不存在")
    return item


def select_tree(window: Win32Window, path: tuple[int, ...]) -> None:
    tree = window.control(IDC_TREE)
    for depth in range(1, len(path)):
        parent = tree_item(window, path[:depth])
        window.send(tree, TVM_EXPAND, TVE_EXPAND, parent)
    item = tree_item(window, path)
    window.send(tree, TVM_SELECTITEM, TVGN_CARET, item)
    require(window.send(tree, TVM_GETNEXTITEM, TVGN_CARET, 0) == item,
            f"目录树未选中路径 {path}")


def save(window: Win32Window, data_path: Path) -> dict:
    window.click(IDC_SAVE)
    data = read_data(data_path)
    require(len(data.get("Folders", [])) >= 2,
            "保存结果缺少隔离数据中的两个目录，疑似使用错误的数据路径")
    return data


def click_tab(window: Win32Window, control_id: int, index: int) -> None:
    tab = window.control(control_id)
    dpi = window.user32.GetDpiForWindow(tab)
    scale = dpi / 96.0
    x, y = round((34 + index * 68) * scale), round(15 * scale)
    position = (y << 16) | x
    window.send(tab, WM_LBUTTONDOWN, 1, position)
    window.send(tab, WM_LBUTTONUP, 0, position)
    require(window.send(tab, TCM_GETCURSEL) == index,
            f"真实标签控件未切换到索引 {index}")


def set_body_type(window: Win32Window, index: int) -> None:
    combo = window.control(IDC_BODY_TYPE)
    require(window.send(combo, CB_SETCURSEL, index) == index,
            "请求体类型下拉框不接受选择")
    window.send(window.hwnd, WM_COMMAND,
                (CBN_SELCHANGE << 16) | IDC_BODY_TYPE, combo)


def find_cell_editor(window: Win32Window) -> int:
    child = window.user32.GetWindow(window.hwnd, GW_CHILD)
    while child:
        if window.user32.GetDlgCtrlID(child) == 0 and class_name(window, child) == "Edit":
            return child
        child = window.user32.GetWindow(child, GW_HWNDNEXT)
    return 0


def edit_entry(window: Win32Window, process, row: int, column: int,
               value: str, timeout: float) -> None:
    post(window, window.hwnd, WM_EDIT_ENTRY_CELL, row, column)
    editor = until("参数表真实单元格编辑器", timeout,
                   lambda: find_cell_editor(window), process)
    set_hwnd_text(window, editor, value)
    window.send(editor, WM_KEYDOWN, VK_RETURN)
    until("参数单元格提交", timeout,
          lambda: not window.user32.IsWindow(editor), process)


def choose_post_method(window: Win32Window, process, timeout: float) -> None:
    # The method selector uses TrackPopupMenu(TPM_RETURNCMD). Queue native
    # key events into that menu rather than altering the button caption.
    window.user32.SetForegroundWindow(window.hwnd)
    post(window, window.control(IDC_METHOD), BM_CLICK)
    menu = until("请求方法菜单", timeout,
                 lambda: top_level(window, "#32768"), process)
    # Menu message loops process queued key events even when the test runner
    # cannot move the physical cursor in a restricted desktop session.
    for code in (VK_DOWN, VK_DOWN, VK_RETURN):
        post(window, menu, WM_KEYDOWN, code)
    until("请求方法菜单关闭", timeout,
          lambda: not top_level(window, "#32768"), process)
    actual = window.text(IDC_METHOD)
    require(actual == "POST", f"方法菜单选中 {actual!r}，期望 'POST'")


def minimize_and_restore(window: Win32Window, process,
                         expected_response: str, timeout: float) -> None:
    post(window, window.hwnd, WM_SYSCOMMAND, SC_MINIMIZE)
    until("正式窗口最小化", timeout,
          lambda: window.user32.IsIconic(window.hwnd), process)
    require(process.poll() is None, "最小化后正式 EXE 意外退出")
    post(window, window.hwnd, WM_SYSCOMMAND, SC_RESTORE)
    until("正式窗口恢复", timeout,
          lambda: not window.user32.IsIconic(window.hwnd)
          and window.user32.IsWindowVisible(window.hwnd), process)
    require(window.text(IDC_RESPONSE_BODY) == expected_response,
            "窗口恢复后真实响应正文丢失")


def close_all_tabs(window: Win32Window, process, timeout: float,
                   before_select=None) -> None:
    tabs = window.control(IDC_REQUEST_TABS)
    count = window.send(tabs, TCM_GETITEMCOUNT)
    require(count >= 2, f"批量关闭前只发现 {count} 个真实请求标签")
    # A keyboard context-menu request uses the selected native tab and opens
    # the same production menu as a user's right click.
    post(window, tabs, WM_CONTEXTMENU, tabs, -1)
    menu = until("请求标签菜单", timeout,
                 lambda: top_level(window, "#32768"), process)
    if before_select is not None:
        before_select()
    for code in (VK_DOWN, VK_RETURN):  # First item: 关闭全部
        post(window, menu, WM_KEYDOWN, code)
    until("请求标签菜单关闭", timeout,
          lambda: not top_level(window, "#32768"), process)
    until("全部请求标签关闭", timeout,
          lambda: window.send(tabs, TCM_GETITEMCOUNT) == 0, process)
    require(window.text(IDC_URL) == "" and process.poll() is None,
            "批量关闭后编辑器未清空或正式 EXE 已退出")


def close_tabs_during_delayed_request(window: Win32Window, process,
                                      data_path: Path, live_base_url: str,
                                      dialog_timeout: float,
                                      request_timeout: float) -> None:
    # Three tabs are open here (seed, original, copy). Send a real delayed GET
    # from the seed tab, then enter the native "close all" menu immediately.
    select_tree(window, (0, 1))
    require(window.text(IDC_URL) == SEED_URL,
            "关闭标签测试未打开隔离数据中的预置接口")
    delay_url = live_base_url.rstrip("/") + "/delay/3"
    window.set_text(IDC_URL, delay_url)
    data = save(window, data_path)
    require(source_folder(data)["Requests"][0]["Url"] == delay_url,
            "延迟请求 URL 未保存到隔离数据文件")
    window.click(IDC_SEND)
    wait_real_transport(window, process, "飞行中关闭标签", request_timeout)
    # Recheck after the native menu opens and immediately before choosing its
    # Close All item. A completed 200 response cannot satisfy this scenario.
    close_all_tabs(
        window, process, dialog_timeout,
        before_select=lambda: require_real_transport(
            window, process, "选择关闭全部菜单项"))
    select_tree(window, (0,))
    select_tree(window, (0, 1))
    until("关闭全部后重开预置接口", dialog_timeout,
          lambda: window.text(IDC_URL) == delay_url, process)
    require(process.poll() is None, "飞行中关闭标签后正式 EXE 意外退出")

    # The second real request catches a stale worker or dead editor after the
    # first tab was closed while its request was active.
    window.set_text(IDC_URL, live_base_url.rstrip("/") + "/get?after=close")
    window.click(IDC_SEND)
    wait_http(window, process, 200, '"after":"close"', request_timeout)
    window.set_text(IDC_URL, SEED_URL)
    data = save(window, data_path)
    seed = source_folder(data)["Requests"][0]
    require(seed["Id"] == SEED_REQUEST_ID and seed["Url"] == SEED_URL,
            "关闭标签测试后未恢复预置接口数据")


def cancel_delayed_request(window: Win32Window, process, data_path: Path,
                           live_base_url: str, timeout: float) -> None:
    select_tree(window, (0, 0))
    require(window.text(IDC_URL) == SEED_URL,
            "取消测试未打开隔离数据中的预置接口")
    delay_url = live_base_url.rstrip("/") + "/delay/3"
    window.set_text(IDC_URL, delay_url)
    window.click(IDC_SEND)
    wait_real_transport(window, process, "取消真实延迟请求", timeout)
    require_real_transport(window, process, "点击取消按钮")
    window.click(IDC_CANCEL)
    until("真实网络请求取消完成", timeout,
          lambda: window.text(IDC_SUMMARY).startswith("已取消"), process)
    require(process.poll() is None, "取消真实网络请求后正式 EXE 意外退出")

    # A second real request proves the tab and HTTP worker remain usable.
    window.set_text(IDC_URL, live_base_url.rstrip("/") + "/get?after=cancel")
    window.click(IDC_SEND)
    wait_http(window, process, 200, '"after":"cancel"', timeout)
    window.set_text(IDC_URL, SEED_URL)
    data = save(window, data_path)
    seed = source_folder(data)["Requests"][0]
    require(seed["Id"] == SEED_REQUEST_ID and seed["Url"] == SEED_URL,
            "取消测试后未恢复隔离磁盘中的预置接口")


def count_requests(folder: dict) -> int:
    return len(folder.get("Requests", [])) + sum(
        count_requests(child) for child in folder.get("Children", []))


def run(args: argparse.Namespace) -> None:
    require(sys.platform == "win32", "核心端到端测试需要 Windows 交互式桌面")
    exe = args.exe.resolve()
    require(exe.is_file(), f"找不到正式 EXE: {exe}")
    live_base = urlsplit(args.live_base_url)
    require(live_base.scheme in ("http", "https") and bool(live_base.hostname)
            and not live_base.query and not live_base.fragment,
            "--live-base-url 必须是不带查询或片段的真实 HTTP/HTTPS 服务地址")
    request_url = args.live_base_url.rstrip("/") + REQUEST_PATH_QUERY
    stored_request_url = request_url.split("?", 1)[0]
    if args.openapi_url:
        parsed = urlsplit(args.openapi_url)
        require(parsed.scheme == "https" and bool(parsed.hostname),
                "--openapi-url 必须是真实 HTTPS JSON 文档地址")
    with isolated_workspace() as run_dir:
        data_path = run_dir / "data.json"
        seed_data(data_path)
        process = None
        window = None
        try:
            process, window = launch(exe, data_path, args.startup_timeout)
            configure_more_win32(window)
            require(window.text(IDC_URL) == SEED_URL,
                    "生产窗口未加载独立数据文件，测试已停止以保护用户数据")
            require(window.send(window.control(IDC_TREE), TVM_GETNEXTITEM,
                                TVGN_ROOT, 0) != 0, "真实目录树没有根节点")
            report("PASS 正式 EXE 加载独立真实数据文件和目录树")

            select_tree(window, (0,))
            prompt(window, process, IDM_FOLDER_ADD_CHILD, NEW_FOLDER,
                   "新增子目录", args.dialog_timeout)
            data = save(window, data_path)
            children = source_folder(data)["Children"]
            require(len(children) == 1 and children[0]["Name"] == NEW_FOLDER,
                    "GUI 新建子目录未保存到真实文件: "
                    f"来源={children!r}, 目标={target_folder(data)['Children']!r}")
            created_folder_id = children[0]["Id"]
            require(created_folder_id not in (SEED_FOLDER_ID, TARGET_FOLDER_ID, ""),
                    "新目录未获得独立 ID")
            report("PASS 目录树新建子目录并保存")

            select_tree(window, (0, 0))
            prompt(window, process, IDM_FOLDER_RENAME, RENAMED_FOLDER,
                   "重命名目录", args.dialog_timeout)
            data = save(window, data_path)
            nested = source_folder(data)["Children"][0]
            require(nested["Id"] == created_folder_id and
                    nested["Name"] == RENAMED_FOLDER,
                    "目录重命名没有保留身份并保存新名称")
            report("PASS 目录重命名及身份保留")

            select_tree(window, (0, 0))
            prompt(window, process, IDM_FOLDER_ADD_REQUEST, NEW_REQUEST,
                   "新增接口", args.dialog_timeout)
            require(window.text(IDC_URL) == "", "新建接口未在正式编辑器打开")
            choose_post_method(window, process, args.dialog_timeout)
            window.set_text(IDC_URL, request_url)
            click_tab(window, IDC_EDITOR_TABS, 1)
            edit_entry(window, process, 0, 1, "X-Core-E2E", args.dialog_timeout)
            edit_entry(window, process, 0, 2, "real-gui", args.dialog_timeout)
            click_tab(window, IDC_EDITOR_TABS, 2)
            set_body_type(window, 1)  # JSON
            window.set_text(IDC_BODY, BODY)
            data = save(window, data_path)
            nested = source_folder(data)["Children"][0]
            requests = nested["Requests"]
            require(len(requests) == 1, "正式 GUI 新建接口数量错误")
            request = requests[0]
            request_id = request["Id"]
            require(request["Name"] == NEW_REQUEST and request["Method"] == "POST",
                    "接口名称或方法未从 GUI 正确保存")
            require(request["Url"] == stored_request_url,
                    f"URL 基础路径没有保存: {request['Url']!r}")
            require([(x["Key"], x["Value"]) for x in request["QueryParams"]
                     if x["IsEnabled"]] == [("case", "real"), ("mode", "post")],
                    "URL 查询参数未进入真实参数表或保存")
            require(any(x["Key"] == "X-Core-E2E" and x["Value"] == "real-gui"
                        for x in request["Headers"]),
                    "GUI Headers 表格输入未保存")
            require(request["BodyType"] == "JSON" and request["BodyContent"] == BODY,
                    "GUI JSON 请求体未保存")
            report("PASS 新建接口、POST、Query、Headers、JSON Body 和真实磁盘持久化")

            window.click(IDC_SEND)
            response = wait_http(window, process, 200, '"method":"POST"',
                                 args.request_timeout)
            try:
                echoed = json.loads(response)
            except ValueError as error:
                raise E2EFailure(f"真实服务没有返回可解析的 JSON: {error}") from error
            echoed_headers = {str(key).lower(): value
                              for key, value in echoed.get("headers", {}).items()}
            require(echoed.get("method") == "POST" and
                    echoed.get("args") == {"case": "real", "mode": "post"} and
                    echoed_headers.get("x-core-e2e") == "real-gui" and
                    echoed.get("json") == json.loads(BODY),
                    f"真实 HTTPS 服务未收到 GUI 配置的完整请求: {response[:400]!r}")
            report("PASS 正式 GUI 向真实 HTTPS 服务发送 POST 并回显 Query、Header、JSON Body")
            minimize_and_restore(window, process, response, args.dialog_timeout)
            report("PASS 正式窗口最小化并恢复后保留真实响应且进程存活")

            # Saving a case uses the production menu command and prompt. It
            # creates a separate snapshot with its own stable ID.
            prompt(window, process, IDM_SAVE_CASE, CASE_NAME,
                   "保存用例", args.dialog_timeout)
            data = read_data(data_path)
            request = source_folder(data)["Children"][0]["Requests"][0]
            require(len(request["Cases"]) == 1 and request["Cases"][0]["Name"] == CASE_NAME,
                    "保存用例未写入正式数据文件")
            case_id = request["Cases"][0]["Id"]
            require(case_id and case_id != request_id and
                    request["Cases"][0]["Method"] == "POST" and
                    request["Cases"][0]["BodyContent"] == BODY and
                    request["Cases"][0]["ResponseSummary"].startswith("200 "),
                    "用例快照没有独立 ID 或丢失编辑器内容")
            report("PASS 保存请求用例并核对完整快照")

            select_tree(window, (0, 0, 0))
            prompt(window, process, IDM_REQUEST_RENAME, RENAMED_REQUEST,
                   "重命名接口", args.dialog_timeout)
            data = save(window, data_path)
            request = source_folder(data)["Children"][0]["Requests"][0]
            require(request["Id"] == request_id and request["Name"] == RENAMED_REQUEST,
                    "接口重命名没有保留身份")
            report("PASS 接口重命名及身份保留")

            select_tree(window, (0, 0, 0))
            command(window, IDM_REQUEST_DUPLICATE)
            until("接口复制打开新标签", args.dialog_timeout,
                  lambda: window.text(IDC_URL) == request_url, process)
            data = save(window, data_path)
            requests = source_folder(data)["Children"][0]["Requests"]
            require(len(requests) == 2, "复制接口未新增记录")
            copied = next((x for x in requests if x["Id"] != request_id), None)
            require(copied is not None and copied["Name"] == RENAMED_REQUEST + " - 副本"
                    and copied["BodyContent"] == BODY and len(copied["Cases"]) == 1,
                    "复制接口未完整复制请求体和用例")
            copied_id = copied["Id"]
            require(copied["Cases"][0]["Id"] != case_id,
                    "复制接口的用例没有生成新身份")
            report("PASS 复制接口及用例身份隔离")
            close_tabs_during_delayed_request(
                window, process, data_path, args.live_base_url,
                args.dialog_timeout, args.request_timeout)
            select_tree(window, (0, 0, 0))
            until("关闭标签后重新打开接口", args.dialog_timeout,
                  lambda: window.text(IDC_URL) == request_url, process)
            report("PASS /delay/3 已建立 TCP 期间批量关闭多个标签、重开目录并继续真实请求")

            # Source nested folder is excluded from the move picker; the two
            # choices are source root and target root, in that order.
            select_tree(window, (0, 0, 1))
            command(window, IDM_REQUEST_MOVE)
            picker = until("移动接口目录选择器", args.dialog_timeout,
                           lambda: top_level(window, "FeatherApiFolderPicker", "移动接口"),
                           process)
            combo = window.user32.GetDlgItem(picker, SMALL_DIALOG_COMBO)
            require(bool(combo) and window.send(combo, CB_GETCOUNT) == 2,
                    "移动接口目录选择器没有显示两个真实目标目录")
            require(window.send(combo, CB_SETCURSEL, 1) == 1,
                    "无法选择目标目录")
            window.send(window.user32.GetDlgItem(picker, IDOK), BM_CLICK)
            until("移动接口对话框关闭", args.dialog_timeout,
                  lambda: not window.user32.IsWindow(picker), process)
            data = save(window, data_path)
            require(len(source_folder(data)["Children"][0]["Requests"]) == 1 and
                    any(x["Id"] == copied_id for x in target_folder(data)["Requests"]),
                    "GUI 移动接口未从来源移除并写入目标目录")
            report("PASS 移动接口到另一个真实目录")

            select_tree(window, (1, 0))
            confirm(window, process, IDM_REQUEST_DELETE, "删除接口",
                    args.dialog_timeout)
            data = save(window, data_path)
            require(not target_folder(data)["Requests"], "确认删除后复制接口仍在磁盘")
            report("PASS 确认删除接口")

            select_tree(window, (0, 0, 0, 0))
            confirm(window, process, IDM_CASE_DELETE, "删除用例",
                    args.dialog_timeout)
            data = save(window, data_path)
            request = source_folder(data)["Children"][0]["Requests"][0]
            require(request["Id"] == request_id and not request["Cases"],
                    "确认删除用例后磁盘仍包含该用例")
            report("PASS 确认删除请求用例")

            select_tree(window, (0, 0))
            confirm(window, process, IDM_FOLDER_DELETE, "删除目录",
                    args.dialog_timeout)
            data = save(window, data_path)
            require(not source_folder(data)["Children"] and
                    source_folder(data)["Requests"][0]["Id"] == SEED_REQUEST_ID,
                    "删除子目录未级联删除其接口，或影响了原有接口")
            report("PASS 确认删除含接口的子目录并保留其他数据")
            cancel_delayed_request(window, process, data_path,
                                   args.live_base_url, args.request_timeout)
            report("PASS /delay/3 已建立 TCP 请求取消后正式 GUI 可再次发送并正常保存")

            imported_count = 0
            if args.openapi_url:
                select_tree(window, (1,))
                before = count_requests(target_folder(read_data(data_path)))
                prompt(window, process, IDM_FOLDER_IMPORT, args.openapi_url,
                       "导入 OpenAPI/Swagger", args.dialog_timeout)
                result_dialog = until("真实 OpenAPI 下载与导入结果", args.import_timeout,
                                      lambda: top_level(window, "#32770"), process)
                dialog_title = title(window, result_dialog)
                require(dialog_title == "导入 OpenAPI/Swagger",
                        f"真实 OpenAPI 导入失败，对话框标题: {dialog_title!r}; "
                        f"子控件={child_descriptions(window, result_dialog)!r}")
                accept = child_button(window, result_dialog, "确定")
                require(bool(accept),
                        f"OpenAPI 结果对话框 {dialog_title!r} 没有确定按钮；"
                        f"子控件={child_descriptions(window, result_dialog)!r}")
                window.send(accept, BM_CLICK)
                until("OpenAPI 结果对话框关闭", args.dialog_timeout,
                      lambda: not window.user32.IsWindow(result_dialog), process)
                imported = count_requests(target_folder(read_data(data_path)))
                require(imported > before,
                        "生产程序报告导入成功，但真实磁盘中没有新增接口")
                imported_count = imported
                report(f"PASS 正式 EXE 从真实 HTTPS OpenAPI 文档导入 {imported-before} 个接口")
            else:
                report("SKIP OpenAPI 网络导入（传入 --openapi-url 启用真实线上文档）")

            stop(process, window, args.shutdown_timeout)
            process = None
            process, window = launch(exe, data_path, args.startup_timeout)
            configure_more_win32(window)
            require(window.text(IDC_URL) == SEED_URL,
                    "重启后未从真实隔离磁盘恢复原有接口")
            data = read_data(data_path)
            require(not source_folder(data)["Children"] and
                    count_requests(target_folder(data)) == imported_count,
                    "重启后目录/接口删除状态不一致")
            report("PASS 关闭并重启正式 EXE 后恢复最终目录和接口状态")
            stop(process, window, args.shutdown_timeout)
            process = None
        finally:
            force_stop(process)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=root / "build/release/FeatherApi.exe")
    parser.add_argument("--live-base-url", default="https://httpbin.org",
                        help="真实 HTTP 回显服务根地址，默认 https://httpbin.org")
    parser.add_argument("--openapi-url", default="",
                        help="真实线上 HTTPS OpenAPI JSON 地址；不传时跳过网络导入")
    parser.add_argument("--startup-timeout", type=float, default=15)
    parser.add_argument("--dialog-timeout", type=float, default=10)
    parser.add_argument("--request-timeout", type=float, default=45)
    parser.add_argument("--import-timeout", type=float, default=120)
    parser.add_argument("--shutdown-timeout", type=float, default=10)
    args = parser.parse_args()
    try:
        run(args)
    except (E2EFailure, OSError) as error:
        report(f"FAIL {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
