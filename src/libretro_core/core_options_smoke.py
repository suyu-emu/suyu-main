"""Check the option descriptors exported by a built libretro core DLL."""
import ctypes
import pathlib
import sys


class Variable(ctypes.Structure):
    _fields_ = [("key", ctypes.c_char_p), ("value", ctypes.c_char_p)]


core = ctypes.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
Environment = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
options = {}


@Environment
def environment(command, data):
    if command == 16:  # RETRO_ENVIRONMENT_SET_VARIABLES
        variables = ctypes.cast(data, ctypes.POINTER(Variable))
        index = 0
        while variables[index].key:
            options[variables[index].key.decode()] = variables[index].value.decode()
            index += 1
    return False


core.retro_set_environment.argtypes = [Environment]
core.retro_set_environment(environment)
assert len(options) == 9, options
# The performance readout is the one option applied live, without a reload.
assert options.pop("suyu_show_perf") == "Show Game Performance; On|Off"
assert all("(reload content)" in description for description in options.values())
for unsupported in ("suyu_renderer", "suyu_online_server", "suyu_online_port",
                    "suyu_online_nickname"):
    assert unsupported not in options
print("CORE_OPTIONS_MATCH_IMPLEMENTED_BEHAVIOR=PASS")
