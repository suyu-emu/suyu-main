"""Run separately: python lifecycle_smoke.py <core DLL> <mode>.

The core uses a portable ``user`` directory beside its host executable. A
temporary host is essential on Windows: changing APPDATA does not redirect
SHGetKnownFolderPath(FOLDERID_RoamingAppData).
"""

import ctypes
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import traceback


def run_isolated(core_path: pathlib.Path, mode: str) -> int:
    with tempfile.TemporaryDirectory(prefix="suyu-libretro-lifecycle-") as temporary:
        profile = pathlib.Path(temporary)
        (profile / "user").mkdir()
        environment = os.environ.copy()
        for variable in ("APPDATA", "LOCALAPPDATA", "HOME", "XDG_DATA_HOME",
                         "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
            directory = profile / variable.lower()
            directory.mkdir()
            environment[variable] = str(directory)
        for variable in ("SUYU_LIBRETRO_TAS_DIR", "SUYU_LIBRETRO_UPDATE_PATH",
                         "SUYU_LIBRETRO_APP_VERSION", "SUYU_LIBRETRO_DISPLAY_VERSION"):
            environment.pop(variable, None)

        interpreter = pathlib.Path(sys.executable)
        if os.name == "nt":
            # GetExeDirectory() chooses the portable user directory beside this
            # copy, before the core can open a profile or adopt any keys.
            interpreter = profile / "suyu_smoke_python.exe"
            shutil.copy2(sys.executable, interpreter)
            for name in (f"python{sys.version_info.major}{sys.version_info.minor}.dll",
                         f"python{sys.version_info.major}.dll"):
                source = pathlib.Path(sys.base_prefix) / name
                if source.is_file():
                    shutil.copy2(source, profile / name)
            environment["PYTHONHOME"] = sys.base_prefix

        return subprocess.run(
            [str(interpreter), str(pathlib.Path(__file__).resolve()),
             "--isolated-child", str(core_path), mode],
            cwd=profile, env=environment, check=False, timeout=60,
        ).returncode


def run_smoke(core_path: pathlib.Path, mode: str) -> None:
    profile = pathlib.Path.cwd()  # The parent created and owns this directory.
    if os.name == "nt":
        assert pathlib.Path(sys.executable).parent == profile
    assert (profile / "user").is_dir()
    tas_directory = profile / "synthetic-tas"
    missing_update = profile / "missing-update.nsp"
    if mode == "tas-retry":
        tas_directory.mkdir()
        # An existing directory without script0-1.txt reaches the TAS request
        # check before the generic loader sees this synthetic NRO header.
        os.environ["SUYU_LIBRETRO_TAS_DIR"] = str(tas_directory)
    elif mode in ("update-retry", "synthetic-header-key"):
        os.environ["SUYU_LIBRETRO_UPDATE_PATH"] = str(missing_update)
    key_modes = ("empty-key", "zero-key", "equal-half-key", "synthetic-header-key")
    if mode in key_modes:
        keys = profile / "user" / "keys"
        keys.mkdir()
        key_text = {
            "empty-key": "",
            "zero-key": "header_key = " + "00" * 32 + "\n",
            "equal-half-key": "header_key = " + "01" * 32 + "\n",
            "synthetic-header-key": "header_key = " + bytes(range(32)).hex() + "\n",
        }[mode]
        (keys / "prod.keys").write_text(key_text, encoding="ascii")

    core = ctypes.CDLL(str(core_path))
    Environment = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)

    @Environment
    def environment(command, data):
        return False  # Optional frontend interfaces are unavailable.

    class Game(ctypes.Structure):
        _fields_ = [("path", ctypes.c_char_p), ("data", ctypes.c_void_p),
                    ("size", ctypes.c_size_t), ("meta", ctypes.c_char_p)]

    core.retro_set_environment.argtypes = [Environment]
    core.retro_load_game.argtypes = [ctypes.POINTER(Game)]
    core.retro_load_game.restype = ctypes.c_bool
    core.retro_set_environment(environment)
    core.retro_init()
    suffix = ".xci" if mode in key_modes else \
             ".nso" if mode == "standalone-nso" else ".nro"
    content = profile / ("dummy" + suffix)
    if mode == "invalid":
        content.write_bytes(b"invalid synthetic content")
    elif mode == "standalone-nso":
        content.write_bytes(b"NSO0" + bytes(0xFC))
    elif mode in key_modes:
        content.write_bytes(b"synthetic encrypted-content placeholder")
    elif mode in ("tas-retry", "update-retry"):
        header = bytearray(0x80)
        header[0x10:0x14] = b"NRO0"
        content.write_bytes(header)
    game = Game(os.fsencode(content), None, 0, None)
    assert not core.retro_load_game(ctypes.byref(game))
    if mode == "tas-retry":
        os.environ.pop("SUYU_LIBRETRO_TAS_DIR")
        os.environ["SUYU_LIBRETRO_UPDATE_PATH"] = str(missing_update)
        assert not core.retro_load_game(ctypes.byref(game))
    if mode == "update-retry":
        os.environ.pop("SUYU_LIBRETRO_UPDATE_PATH")
        tas_directory.mkdir()
        os.environ["SUYU_LIBRETRO_TAS_DIR"] = str(tas_directory)
        assert not core.retro_load_game(ctypes.byref(game))
    core.retro_unload_game()
    core.retro_deinit()
    log = (profile / "user" / "log" / "suyu_log.txt").read_text(
        encoding="utf-8", errors="replace")
    if mode == "missing":
        assert "content is not an accessible file" in log
    elif mode == "invalid":
        assert "executable header is not recognized" in log
    elif mode == "standalone-nso":
        assert "standalone NSO requires a deconstructed ExeFS main" in log
        assert "libretro core: loading game" not in log
    elif mode in ("empty-key", "zero-key", "equal-half-key"):
        assert "encrypted content requires a usable NCA header key" in log
        assert "Failed to set IV on OpenSSL contexts" not in log
        assert "libretro core: loading game" not in log
    elif mode == "synthetic-header-key":
        assert "encrypted content requires a usable NCA header key" not in log
        assert "base or update file could not be opened read-only" in log
        assert "Failed to set IV on OpenSSL contexts" not in log
    else:
        assert "executable header is not recognized" not in log
        tas_rejection = log.find("requested TAS script0-1.txt is unavailable")
        update_rejection = log.find("base or update file could not be opened read-only")
        assert tas_rejection >= 0 and update_rejection >= 0
        assert (tas_rejection < update_rejection) == (mode == "tas-retry")
    print("REQUESTED_REJECTION_BRANCH=PASS", flush=True)
    if mode in ("tas-retry", "update-retry"):
        print("CROSS_BRANCH_RETRY_WITHOUT_UNLOAD=PASS", flush=True)
    print("FAILED_CONTENT_DEINIT=PASS", flush=True)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--isolated-child":
        core_argument, selected_mode = sys.argv[2:4]
        try:
            run_smoke(pathlib.Path(core_argument), selected_mode)
        except Exception:
            traceback.print_exc()
            sys.stderr.flush()
            # The core may still have background threads after a failed load;
            # exit the isolated child so the parent can remove its profile.
            os._exit(1)
    else:
        core_argument = pathlib.Path(sys.argv[1]).resolve()
        selected_mode = sys.argv[2] if len(sys.argv) > 2 else "missing"
        if selected_mode not in ("missing", "invalid", "tas-retry", "update-retry",
                                 "standalone-nso", "empty-key", "zero-key", "equal-half-key",
                                 "synthetic-header-key"):
            raise SystemExit(f"unknown smoke mode: {selected_mode}")
        raise SystemExit(run_isolated(core_argument, selected_mode))
