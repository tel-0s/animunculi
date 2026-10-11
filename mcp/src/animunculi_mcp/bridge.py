"""Async client for the Animunculi in-game TCP server (see docs/PROTOCOL.md)."""
from __future__ import annotations

import asyncio
import itertools
import json
import logging
from pathlib import Path
from typing import Any

from . import PROTOCOL_VERSION, __version__
from .config import Config, default_discovery_paths

log = logging.getLogger("animunculi.bridge")

MAX_LINE = 48 * 1024 * 1024
DEFAULT_PORT = 47700


class BridgeError(Exception):
    """An error returned by the plugin, or a transport failure (code ``disconnected``/``timeout``)."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def read_discovery(cfg: Config) -> dict[str, Any]:
    candidates = [cfg.discovery] if cfg.discovery else default_discovery_paths()
    for path in candidates:
        if path and Path(path).is_file():
            try:
                data = json.loads(Path(path).read_text(encoding="utf-8"))
                data["_path"] = str(path)
                return data
            except (OSError, ValueError) as exc:
                log.warning("unreadable discovery file %s: %s", path, exc)
    return {}


class Bridge:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._read_task: asyncio.Task | None = None
        self._pending: dict[int, asyncio.Future] = {}
        self._ids = itertools.count(1)
        self._connect_lock = asyncio.Lock()
        self._write_lock = asyncio.Lock()
        self.hello: dict[str, Any] = {}
        self.endpoint: str = ""

    @property
    def connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()

    async def _open(self) -> None:
        disc = read_discovery(self.cfg)
        host = self.cfg.host or disc.get("host") or "127.0.0.1"
        if host in ("0.0.0.0", "::"):
            host = "127.0.0.1"
        port = self.cfg.port or int(disc.get("port") or DEFAULT_PORT)
        token = self.cfg.token if self.cfg.token is not None else disc.get("token", "")
        self.endpoint = f"{host}:{port}"
        try:
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_connection(host, port, limit=MAX_LINE), self.cfg.connect_timeout)
        except (OSError, asyncio.TimeoutError) as exc:
            raise BridgeError("disconnected",
                              f"cannot reach the game at {self.endpoint} ({exc.__class__.__name__}). "
                              "Is Skyrim running with Animunculi.dll loaded?") from None
        self._read_task = asyncio.create_task(self._read_loop(self._reader, self._writer))
        try:
            self.hello = await self._request("hello", {
                "token": token or "",
                "client": f"animunculi-mcp/{__version__}",
                "protocol": PROTOCOL_VERSION,
            }, self.cfg.connect_timeout)
        except BridgeError:
            await self.close()
            raise
        log.info("connected to %s: %s", self.endpoint, self.hello)

    async def ensure_connected(self) -> None:
        if self.connected:
            return
        async with self._connect_lock:
            if not self.connected:
                await self._open()

    async def close(self) -> None:
        writer, self._writer = self._writer, None
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass
        if self._read_task is not None:
            self._read_task.cancel()
            self._read_task = None
        self._fail_pending(BridgeError("disconnected", "connection closed"))

    def _fail_pending(self, exc: BridgeError) -> None:
        pending, self._pending = self._pending, {}
        for fut in pending.values():
            if not fut.done():
                fut.set_exception(exc)

    async def _read_loop(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    msg = json.loads(line)
                except ValueError:
                    log.warning("malformed line from game: %r", line[:200])
                    continue
                fut = self._pending.pop(msg.get("id"), None)
                if fut is None or fut.done():
                    continue
                if "error" in msg:
                    err = msg["error"] or {}
                    fut.set_exception(BridgeError(err.get("code", "failed"), err.get("message", "")))
                else:
                    fut.set_result(msg.get("result") or {})
        except (OSError, asyncio.IncompleteReadError, ValueError) as exc:
            log.warning("read loop ended: %s", exc)
        except asyncio.CancelledError:
            return
        finally:
            writer.close()
            if self._writer is writer:  # a newer connection may already have replaced this one
                self._writer = None
                self._fail_pending(BridgeError("disconnected", "the game closed the connection"))

    async def _request(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
        if self._writer is None:
            raise BridgeError("disconnected", "not connected")
        req_id = next(self._ids)
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self._pending[req_id] = fut
        payload = json.dumps({"id": req_id, "method": method, "params": params},
                             ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
        try:
            async with self._write_lock:
                self._writer.write(payload)
                await self._writer.drain()
        except OSError as exc:
            self._pending.pop(req_id, None)
            await self.close()
            raise BridgeError("disconnected", str(exc)) from None
        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            self._pending.pop(req_id, None)
            raise BridgeError("timeout", f"'{method}' got no answer within {timeout:.0f}s") from None

    async def call(self, method: str, params: dict[str, Any] | None = None,
                   timeout: float | None = None) -> dict[str, Any]:
        """Call a plugin method, reconnecting once if the previous connection went away."""
        params = {k: v for k, v in (params or {}).items() if v is not None}
        timeout = timeout or self.cfg.call_timeout
        for attempt in (0, 1):
            await self.ensure_connected()
            try:
                return await self._request(method, params, timeout)
            except BridgeError as exc:
                if exc.code == "disconnected" and attempt == 0:
                    await self.close()
                    continue
                raise
        raise BridgeError("disconnected", "unreachable")  # pragma: no cover
