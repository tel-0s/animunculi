"""Cross-language check: the Python bridge/MCP against the real C++ server (headless mode of the
native test binary). Skipped when the binary has not been built."""
from __future__ import annotations

import asyncio
import json
import socket
import subprocess
from pathlib import Path

import pytest

from animunculi_mcp.bridge import Bridge, BridgeError
from animunculi_mcp.config import Config
from animunculi_mcp.server import build_server

EXE = Path(__file__).resolve().parents[2] / "build/vs2026/Release/animunculi_tests.exe"
pytestmark = pytest.mark.skipif(not EXE.exists(), reason="native test binary not built")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture()
def native_server():
    port = _free_port()
    proc = subprocess.Popen([str(EXE), "--serve", str(port), "tok"], stdout=subprocess.PIPE, text=True)
    try:
        line = proc.stdout.readline()
        assert "serving on" in line, line
        yield port
    finally:
        proc.kill()
        proc.wait(5)


def test_bridge_roundtrip_against_cpp(native_server):
    async def main():
        bridge = Bridge(Config(host="127.0.0.1", port=native_server, token="tok"))
        pong = await bridge.call("ping")
        big = "ü" * 300_000  # multi-byte UTF-8, several TCP segments
        echo = await bridge.call("echo", {"text": big, "n": [1, 2.5, None, True]})
        # pipelining: a long-poll must not block other requests on the same connection
        waiter = asyncio.create_task(bridge.call("wait_events", {"since": bridge.hello["event_seq"],
                                                                 "types": ["player_chat"], "timeout_ms": 5000}))
        await bridge.call("emit", {"type": "noise", "delay_ms": 50})
        await bridge.call("emit", {"type": "player_chat", "data": {"text": "hej"}, "delay_ms": 150})
        got = await asyncio.wait_for(waiter, 10)
        await bridge.close()
        return pong, echo, got, bridge.hello
    pong, echo, got, hello = asyncio.run(main())
    assert pong == {"pong": True}
    assert echo["text"] == "ü" * 300_000 and echo["n"] == [1, 2.5, None, True]
    assert [e["type"] for e in got["events"]] == ["player_chat"] and got["events"][0]["text"] == "hej"
    assert hello["protocol"] == 1 and hello["plugin"] == "Animunculi"


def test_bad_token_against_cpp(native_server):
    async def main():
        bridge = Bridge(Config(host="127.0.0.1", port=native_server, token="nope"))
        with pytest.raises(BridgeError) as info:
            await bridge.call("ping")
        return info.value.code
    assert asyncio.run(main()) == "unauthorized"


def test_mcp_wait_for_events_against_cpp(native_server):
    async def main():
        mcp, session = build_server(Config(host="127.0.0.1", port=native_server, token="tok"))
        status = json.loads((await mcp.call_tool("game_status", {})).content[0].text)
        await session.call("emit", {"type": "heard", "data": {"text": "Halt!"}, "delay_ms": 100})
        res = json.loads((await mcp.call_tool("wait_for_events", {"timeout_s": 5})).content[0].text)
        return status, res
    status, res = asyncio.run(main())
    assert status["headless"] is True
    assert any(e["type"] == "heard" and e["text"] == "Halt!" for e in res["events"])
