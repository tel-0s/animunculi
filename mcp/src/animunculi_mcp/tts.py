"""Voice clip sourcing: take audio generated elsewhere, or synthesize it through a configured provider.

Providers:
  none     no synthesis; `say` shows subtitles only unless an audio file is supplied
  command  run a shell command that writes a WAV to {out}; the text arrives on stdin and in {text_file}
  openai   POST to an OpenAI-compatible /v1/audio/speech endpoint (OpenAI, Kokoro-FastAPI,
           openedai-speech, LocalAI, ...) requesting WAV output

Whatever the source, the result is normalised to a RIFF WAV the plugin can parse, using ffmpeg when
the input is some other container or codec.
"""
from __future__ import annotations

import asyncio
import json
import shutil
import struct
import subprocess
import tempfile
import urllib.request
from pathlib import Path

from .config import Config


class TTSError(Exception):
    pass


_WAV_FORMATS = {1: "pcm", 3: "float", 0xFFFE: "extensible"}


def wav_info(data: bytes) -> dict | None:
    """Minimal RIFF/WAVE header parse. Returns None if the data is not a WAV the plugin accepts."""
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    pos, fmt, data_len = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt " and size >= 16:
            tag, ch, rate, _br, _align, bits = struct.unpack_from("<HHIIHH", data, body)
            if tag == 0xFFFE and size >= 40:
                tag = struct.unpack_from("<H", data, body + 24)[0]  # SubFormat GUID's first word
            fmt = {"tag": tag, "channels": ch, "rate": rate, "bits": bits}
        elif cid == b"data":
            data_len = min(size, len(data) - body)
            break
        pos = body + size + (size & 1)
    if not fmt or data_len is None:
        return None
    ok = (fmt["tag"] == 1 and fmt["bits"] in (8, 16, 24, 32)) or (fmt["tag"] == 3 and fmt["bits"] == 32)
    if not ok or fmt["channels"] not in (1, 2) or not 4000 <= fmt["rate"] <= 192000:
        return None
    frame = fmt["channels"] * fmt["bits"] // 8
    fmt["duration_s"] = data_len / frame / fmt["rate"]
    return fmt


def _ffmpeg_to_wav(cfg: Config, data: bytes) -> bytes:
    exe = shutil.which(cfg.ffmpeg) or (cfg.ffmpeg if Path(cfg.ffmpeg).is_file() else None)
    if not exe:
        raise TTSError("audio is not a PCM WAV and ffmpeg was not found to convert it "
                       "(set ANIMUNCULI_FFMPEG)")
    proc = subprocess.run([exe, "-hide_banner", "-loglevel", "error", "-i", "pipe:0",
                           "-ac", "1", "-ar", "44100", "-c:a", "pcm_s16le", "-f", "wav", "pipe:1"],
                          input=data, capture_output=True, timeout=60)
    if proc.returncode != 0 or not wav_info(proc.stdout):
        raise TTSError(f"ffmpeg could not convert the audio: {proc.stderr.decode(errors='replace')[:300]}")
    return proc.stdout


def normalise(cfg: Config, data: bytes) -> bytes:
    if wav_info(data):
        return data
    return _ffmpeg_to_wav(cfg, data)


def load_audio_file(cfg: Config, path: str) -> bytes:
    p = Path(path).expanduser()
    if not p.is_file():
        raise TTSError(f"audio file not found: {p}")
    return normalise(cfg, p.read_bytes())


def _synth_command(cfg: Config, text: str, voice: str | None) -> bytes:
    """Run the configured command. The spoken text never touches the command line (no shell
    injection): it is piped to stdin, and also written to a file available as {text_file}."""
    with tempfile.TemporaryDirectory(prefix="animunculi-tts-") as tmp:
        out = Path(tmp) / "line.wav"
        text_file = Path(tmp) / "line.txt"
        text_file.write_text(text, encoding="utf-8")
        cmd = (cfg.tts_command.replace("{out}", str(out)).replace("{text_file}", str(text_file))
               .replace("{voice}", voice or cfg.tts_voice))
        proc = subprocess.run(cmd, shell=True, input=text.encode("utf-8"),
                              capture_output=True, timeout=cfg.tts_timeout)
        if proc.returncode != 0 or not out.is_file():
            raise TTSError(f"TTS command failed ({proc.returncode}): "
                           f"{proc.stderr.decode(errors='replace')[-400:]}")
        return normalise(cfg, out.read_bytes())


def _synth_openai(cfg: Config, text: str, voice: str | None) -> bytes:
    body = json.dumps({"model": cfg.tts_model, "input": text, "voice": voice or cfg.tts_voice,
                       "response_format": "wav"}).encode("utf-8")
    req = urllib.request.Request(cfg.tts_url, data=body, method="POST",
                                 headers={"Content-Type": "application/json"})
    if cfg.tts_api_key:
        req.add_header("Authorization", f"Bearer {cfg.tts_api_key}")
    try:
        with urllib.request.urlopen(req, timeout=cfg.tts_timeout) as resp:
            data = resp.read()
    except OSError as exc:
        raise TTSError(f"TTS request to {cfg.tts_url} failed: {exc}") from None
    return normalise(cfg, _fix_streamed_wav(data))


def _fix_streamed_wav(data: bytes) -> bytes:
    """Streaming TTS servers often write 0xFFFFFFFF/0 chunk sizes; patch them to the real length."""
    if len(data) >= 44 and data[:4] == b"RIFF" and data[8:12] == b"WAVE":
        buf = bytearray(data)
        struct.pack_into("<I", buf, 4, len(buf) - 8)
        pos = 12
        while pos + 8 <= len(buf):
            cid, size = bytes(buf[pos:pos + 4]), struct.unpack_from("<I", buf, pos + 4)[0]
            if cid == b"data":
                if size in (0, 0xFFFFFFFF) or pos + 8 + size > len(buf):
                    struct.pack_into("<I", buf, pos + 4, len(buf) - pos - 8)
                break
            pos += 8 + size + (size & 1)
        return bytes(buf)
    return data


async def synthesize(cfg: Config, text: str, voice: str | None = None) -> bytes | None:
    """Return WAV bytes for `text`, or None when no provider is configured."""
    provider = (cfg.tts or "none").lower()
    if provider == "none":
        return None
    if provider == "command":
        if not cfg.tts_command:
            raise TTSError("ANIMUNCULI_TTS=command requires ANIMUNCULI_TTS_COMMAND")
        return await asyncio.to_thread(_synth_command, cfg, text, voice)
    if provider == "openai":
        return await asyncio.to_thread(_synth_openai, cfg, text, voice)
    raise TTSError(f"unknown TTS provider '{cfg.tts}' (expected none, command or openai)")
