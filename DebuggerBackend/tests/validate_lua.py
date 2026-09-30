"""Headless Lua checks with the packaged x64 Lua runtime and mocked CE APIs."""
from __future__ import annotations

import argparse
import ctypes
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lua-dll", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    lua = ctypes.CDLL(str(args.lua_dll.resolve()))
    lua.luaL_newstate.restype = ctypes.c_void_p
    lua.luaL_openlibs.argtypes = [ctypes.c_void_p]
    lua.luaL_loadfilex.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lua.luaL_loadfilex.restype = ctypes.c_int
    lua.lua_pcallk.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ssize_t, ctypes.c_void_p]
    lua.lua_pcallk.restype = ctypes.c_int
    lua.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    lua.lua_tolstring.restype = ctypes.c_char_p
    lua.lua_settop.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lua.lua_pushstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lua.lua_setglobal.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lua.lua_close.argtypes = [ctypes.c_void_p]
    state = lua.luaL_newstate()
    if not state:
        raise RuntimeError("Cannot create the Lua state")
    try:
        lua.luaL_openlibs(state)
        sources = {"backend_factory_file": root / "CheatEngineExecutablePlugin/integration/ksword_hvm.lua",
                   "backend_bootstrap_file": root / "CheatEngineExecutablePlugin/integration/10_ksword_bridge.lua"}
        for name, path in sources.items():
            if lua.luaL_loadfilex(state, str(path).encode(), None):
                raise RuntimeError(lua.lua_tolstring(state, -1, None).decode())
            lua.lua_settop(state, 0)
            lua.lua_pushstring(state, str(path).encode())
            lua.lua_setglobal(state, name.encode())
        tests = Path(__file__).with_name("LuaTests.lua")
        if lua.luaL_loadfilex(state, str(tests).encode(), None) or lua.lua_pcallk(state, 0, 0, 0, 0, None):
            raise RuntimeError(lua.lua_tolstring(state, -1, None).decode())
        return 0
    finally:
        lua.lua_close(state)


if __name__ == "__main__":
    raise SystemExit(main())
