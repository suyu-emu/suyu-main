#!/usr/bin/env python3
"""Close a test-owned Windows GUI normally; never attach to another process.

Use an existing isolated integration user directory and optional synthetic export
fixture. This never builds a native launcher or starts a game. Exit timeout cleanup
is explicitly a failure, not successful shutdown verification.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import socket
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--port', type=int, required=True)
    p.add_argument('--fixture', type=Path, help='Synthetic integration NSP only')
    p.add_argument('--source-output', type=Path, help='Short Source export destination (Windows budget: 70 characters)')
    p.add_argument('--close-order', choices=['modal-main', 'main', 'all'], default='modal-main')
    p.add_argument('--disconnect-fire', action='store_true')
    p.add_argument('--rescan-delay', type=float, default=0)
    p.add_argument('--timeout', type=float, default=90)
    a = p.parse_args()
    if os.name != 'nt':
        p.error('Windows-only integration helper')
    if a.fixture and (not a.fixture.is_file() or a.fixture.suffix.lower() != '.nsp'):
        p.error('--fixture must be an existing synthetic NSP file, not an extracted directory')
    source_output = (a.source_output or (a.work/'source-output')).resolve()
    if a.fixture and len(str(source_output)) > 70:
        p.error('Source output exceeds the 70-character Windows path budget; pass a short --source-output')
    a.work.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(APPDATA=str(a.work / 'appdata'), LOCALAPPDATA=str(a.work / 'localappdata'),
               SUYU_MCP_PORT=str(a.port), SUYU_CMD_CAPTURE_HEADLESS='1')
    pending = []
    def rpc(name, args=None, fire=False):
        s = socket.create_connection(('127.0.0.1', a.port), timeout=2)
        s.sendall((json.dumps({'jsonrpc':'2.0','id':1,'method':'tools/call',
                             'params':{'name':name,'arguments':args or {}}})+'\n').encode())
        if fire:
            if a.disconnect_fire:
                s.close()
            else:
                pending.append(s)
            return None
        with s:
            data = b''
            while b'\n' not in data:
                chunk = s.recv(65536)
                if not chunk:
                    raise RuntimeError('RPC closed without response')
                data += chunk
            obj = json.loads(data.split(b'\n',1)[0])
            if 'error' in obj:
                raise RuntimeError(str(obj['error']))
            result = obj['result']
            if 'content' in result:
                result = json.loads(result['content'][0]['text'])
            return result
    # Match the integration runner's invocation on the release being tested.
    proc = subprocess.Popen([str(a.exe.resolve())], env=env, cwd=str(a.work.resolve()))
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    def creation_time(handle):
        values = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(value) for value in values)):
            raise ctypes.WinError(ctypes.get_last_error())
        return (values[0].dwHighDateTime << 32) | values[0].dwLowDateTime
    owned_creation = creation_time(proc._handle)
    u = ctypes.WinDLL('user32', use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    u.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    u.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
    u.GetWindow.restype = wintypes.HWND
    u.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    def require_owned_window(hwnd):
        if proc.poll() is not None:
            raise RuntimeError('owned GUI already exited before window action')
        pid = wintypes.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value != proc.pid:
            raise RuntimeError('window ownership changed before close')
        handle = kernel.OpenProcess(0x1000, False, pid.value)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            if creation_time(handle) != owned_creation:
                raise RuntimeError('process identity changed before close')
        finally:
            kernel.CloseHandle(handle)
    def windows():
        found = []
        @callback_type
        def collect(hwnd, _):
            pid = wintypes.DWORD()
            u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == proc.pid and u.IsWindowVisible(hwnd):
                found.append((hwnd, u.GetWindow(hwnd, 4)))  # GW_OWNER
            return True
        u.EnumWindows(collect, 0)
        return found
    record = {'pid':proc.pid,'process_creation_time':owned_creation,
              'close_order':a.close_order,'disconnect_fire':a.disconnect_fire,
              'phase':'rpc_readiness','passed':False,
              'source_output':str(source_output)}
    try:
        deadline = time.monotonic()+a.timeout
        while True:
            if proc.poll() is not None:
                raise RuntimeError('GUI exited before RPC readiness')
            try:
                rpc('get_firmware_status')
                break
            except (OSError, RuntimeError, KeyError) as error:
                record['last_rpc_error'] = str(error)
                if time.monotonic()>=deadline:
                    raise RuntimeError('RPC readiness timeout; confirm frontend flags/tool name')
                time.sleep(.1)
        if a.fixture:
            record['phase'] = 'opening_export_modal'
            deadline = time.monotonic()+a.timeout
            rpc('trigger_ui_action', {'action':'export_game'}, fire=True)
            # Opening the modal is asynchronous; status confirms its lifetime.
            while True:
                status = rpc('get_aot_export_status')
                record['last_status'] = status
                if status.get('available'):
                    break
                if time.monotonic()>=deadline:
                    raise RuntimeError('export modal timeout')
                time.sleep(.1)
            record['phase'] = 'starting_source_export'
            record['trigger_result'] = rpc('trigger_ui_action', {'action':'aot_test_export', 'rom_path':str(a.fixture.resolve()),
                'output_dir':str(source_output),'format':'source',
                'backend':'static','package':'reference','include_save':False,'include_shader':False,'include_config':False})
            if record['trigger_result'].get('success') is False:
                raise RuntimeError('Source export trigger rejected')
            record['phase'] = 'waiting_source_export'
            deadline = time.monotonic()+a.timeout
            while True:
                status = rpc('get_aot_export_status')
                record['last_status'] = status
                if not status.get('available'):
                    raise RuntimeError('export modal unavailable while awaiting Source result')
                if status.get('done') and not status.get('running'):
                    if not status.get('success'):
                        raise RuntimeError('synthetic Source export failed')
                    record['export_complete'] = True
                    break
                if time.monotonic()>=deadline:
                    raise RuntimeError('Source export timeout')
                time.sleep(.1)
        record['phase'] = 'enumerating_owned_windows'
        owned = windows()
        main_windows = [h for h, owner in owned if not owner]
        modals = [h for h, owner in owned if owner in main_windows]
        if len(main_windows)!=1:
            raise RuntimeError('expected exactly one visible owned main window')
        if a.close_order=='modal-main':
            record['phase'] = 'closing_modal'
            for hwnd in modals:
                require_owned_window(hwnd)
                if not u.PostMessageW(hwnd, 0x10, 0, 0):
                    raise ctypes.WinError(ctypes.get_last_error())
            time.sleep(a.rescan_delay)
            targets = main_windows
        elif a.close_order=='main':
            targets = main_windows
        else:
            targets = [h for h,_ in owned]
        record['window_count'] = len(owned)
        record['phase'] = 'posting_close'
        for hwnd in targets:
            require_owned_window(hwnd)
            if not u.PostMessageW(hwnd, 0x10, 0, 0):
                raise ctypes.WinError(ctypes.get_last_error())
        record['phase'] = 'waiting_normal_exit'
        code = proc.wait(timeout=15)
        record['exit_code'] = code
        record['passed'] = code==0
        if code!=0:
            raise RuntimeError(f'normal close exit 0x{code & 0xffffffff:08x}')
        record['phase'] = 'normal_exit'
    except Exception as error:
        record['error'] = str(error)
        record['passed'] = False
        record['exit_code_before_cleanup'] = proc.poll()
        raise
    finally:
        for s in pending:
            s.close()
        if proc.poll() is None:
            record['forced_cleanup'] = True
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        (a.work/'shutdown-result.json').write_text(json.dumps(record,indent=2)+'\n')
    return 0

if __name__=='__main__':
    raise SystemExit(main())

