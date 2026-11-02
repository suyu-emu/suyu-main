#!/usr/bin/env python3
"""Non-FUSE, isolated help and offscreen GUI startup checks (no guest)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('artifact', type=Path)
p.add_argument('--report', type=Path, required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='suyu-appimage-smoke-') as tmp:
    root = Path(tmp)
    subprocess.run([str(a.artifact.resolve()), '--appimage-extract'], cwd=root, check=True)
    app = root / 'squashfs-root'
    env = dict(os.environ, HOME=str(root / 'home'), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_DATA_HOME=str(root / 'data'), XDG_CACHE_HOME=str(root / 'cache'),
               QT_QPA_PLATFORM='offscreen', SUYU_MCP_PORT='0')
    cli = subprocess.run([str(app / 'AppRun'), '--suyu-cmd', '--help'], env=env,
                         capture_output=True, text=True, timeout=30)
    if cli.returncode or 'usage' not in (cli.stdout + cli.stderr).lower():
        raise RuntimeError('extracted CLI help failed')
    with (root / 'gui.log').open('w') as log:
        gui = subprocess.Popen([str(app / 'AppRun')], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(8)
            if gui.poll() is not None: raise RuntimeError('offscreen GUI exited during startup')
        finally:
            if gui.poll() is None:
                gui.terminate()  # Only the exact child created above.
                try: gui.wait(timeout=10)
                except subprocess.TimeoutExpired: gui.kill(); gui.wait(timeout=10)
    text = (root / 'gui.log').read_text(errors='replace')
    if any(s in text for s in ('Could not load the Qt platform plugin', 'no Qt platform plugin could be initialized')):
        raise RuntimeError('offscreen Qt plugin failed')
    a.report.write_text(json.dumps(dict(non_fuse_extract=True, cli_help=True,
                                      gui_offscreen_alive_8s=True, gui_shutdown='owned SIGTERM; not a clean shutdown proof'), indent=2) + '\n')
