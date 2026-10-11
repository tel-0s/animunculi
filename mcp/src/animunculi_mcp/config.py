"""Configuration: environment variables, optionally layered over a JSON file.

Every setting can be given as ``ANIMUNCULI_<NAME>`` in the environment (what MCP client configs
usually set) or as ``"<name>"`` in the JSON file named by ``ANIMUNCULI_CONFIG``.
"""
from __future__ import annotations

import json
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path


def _documents_dir() -> Path:
    """The real Documents folder (honours OneDrive/folder redirection on Windows)."""
    if sys.platform == "win32":
        try:
            import ctypes
            from ctypes import wintypes

            class GUID(ctypes.Structure):
                _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                            ("Data3", wintypes.WORD), ("Data4", wintypes.BYTE * 8)]

            # FOLDERID_Documents {FDD39AD0-238F-46AF-ADB4-6C85480369C7}
            fid = GUID(0xFDD39AD0, 0x238F, 0x46AF,
                       (wintypes.BYTE * 8)(0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7))
            out = ctypes.c_wchar_p()
            shell32 = ctypes.windll.shell32
            if shell32.SHGetKnownFolderPath(ctypes.byref(fid), 0, None, ctypes.byref(out)) == 0:
                path = Path(out.value)
                ctypes.windll.ole32.CoTaskMemFree(out)
                return path
        except Exception:  # pragma: no cover - fall through to the heuristic
            pass
    return Path.home() / "Documents"


def default_discovery_paths() -> list[Path]:
    games = _documents_dir() / "My Games"
    return [games / edition / "SKSE" / "Animunculi.discovery.json"
            for edition in ("Skyrim Special Edition", "Skyrim Special Edition GOG", "Skyrim VR")]


@dataclass
class Config:
    host: str | None = None          # None -> discovery file -> 127.0.0.1
    port: int | None = None          # None -> discovery file -> 47700
    token: str | None = None         # None -> discovery file
    discovery: Path | None = None    # explicit discovery file
    agent: str = "default"           # which embodied agent this MCP session drives
    connect_timeout: float = 5.0
    call_timeout: float = 20.0

    tts: str = "none"                # none | command | openai
    tts_command: str = ""            # e.g. 'piper -m en_GB-alba.onnx -f "{out}"' (text on stdin)
    tts_url: str = "http://127.0.0.1:8880/v1/audio/speech"
    tts_model: str = "tts-1"
    tts_voice: str = "alloy"
    tts_api_key: str = ""
    tts_timeout: float = 60.0
    ffmpeg: str = "ffmpeg"

    debug_tools: bool = False        # exposes console_command (the plugin must allow it too)
    default_radius_m: float = 40.0
    max_wait_s: float = 50.0         # MCP hosts commonly time tool calls out at 60 s
    image_max_dim: int = 1024

    extra: dict = field(default_factory=dict)

    @staticmethod
    def load(environ: dict[str, str] | None = None) -> "Config":
        env = dict(os.environ if environ is None else environ)
        data: dict = {}
        cfg_path = env.get("ANIMUNCULI_CONFIG")
        if cfg_path:
            data = json.loads(Path(cfg_path).read_text(encoding="utf-8"))

        cfg = Config()
        for name, current in list(vars(cfg).items()):
            if name == "extra":
                continue
            raw = env.get(f"ANIMUNCULI_{name.upper()}", data.get(name))
            if raw is None or raw == "":
                continue
            setattr(cfg, name, _coerce(name, raw, current))
        cfg.extra = {k: v for k, v in data.items() if not hasattr(cfg, k)}
        return cfg


_INTS = {"port", "image_max_dim"}
_FLOATS = {"connect_timeout", "call_timeout", "tts_timeout", "default_radius_m", "max_wait_s"}
_BOOLS = {"debug_tools"}
_PATHS = {"discovery"}


def _coerce(name: str, raw, current):
    if name in _INTS:
        return int(raw)
    if name in _FLOATS:
        return float(raw)
    if name in _BOOLS:
        return raw if isinstance(raw, bool) else str(raw).strip().lower() in ("1", "true", "yes", "on")
    if name in _PATHS:
        return Path(raw)
    return str(raw)
