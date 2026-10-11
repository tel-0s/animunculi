"""End-to-end tests: the real MCP server talking to the mock game over TCP."""
from __future__ import annotations

import asyncio
import json
import struct

import pytest

from animunculi_mcp.config import Config
from animunculi_mcp.mock_game import MockGame
from animunculi_mcp.server import build_server
from animunculi_mcp import tts
from mcp.server.mcpserver.exceptions import ToolError


def run(coro):
    return asyncio.run(coro)


async def _start(token: str = "secret", **cfg_kw):
    game = MockGame(token=token)
    srv = await asyncio.start_server(game.serve_client, "127.0.0.1", 0, limit=48 * 1024 * 1024)
    port = srv.sockets[0].getsockname()[1]
    cfg = Config(host="127.0.0.1", port=port, token=token, **cfg_kw)
    mcp, session = build_server(cfg)
    return game, srv, mcp, session


def _text(result) -> str:
    assert not result.is_error, result
    return "".join(c.text for c in result.content if c.type == "text")


def _call(mcp, tool_name, **args):
    return mcp.call_tool(tool_name, args)


async def _error(mcp, tool_name, **args) -> str:
    """In-process call_tool raises ToolError (clients receive it as an isError result)."""
    with pytest.raises(ToolError) as info:
        await mcp.call_tool(tool_name, args)
    return str(info.value)


def test_tool_list_is_complete():
    async def main():
        _, srv, mcp, _ = await _start()
        names = {t.name for t in await mcp.list_tools()}
        srv.close()
        return names
    names = run(main())
    for expected in ("observe", "wait_for_events", "say", "see", "move_to", "bind_body", "activate",
                     "give_item", "emote", "perform_mod_action", "quest_log", "inventory"):
        assert expected in names
    assert "console_command" not in names  # debug tools are opt-in


def test_observe_includes_world_and_events():
    async def main():
        game, srv, mcp, _ = await _start()
        await game.emit("heard", speaker="Guard", text="Halt!")
        out = json.loads(_text(await _call(mcp, "observe")))
        srv.close()
        return out
    out = run(main())
    assert out["self"]["name"] == "Lydia"
    assert out["location"]["worldspace"] == "Skyrim"
    assert any(e["type"] == "heard" and e["text"] == "Halt!" for e in out["events"])


def test_bad_token_is_reported():
    async def main():
        game = MockGame(token="right")
        srv = await asyncio.start_server(game.serve_client, "127.0.0.1", 0)
        port = srv.sockets[0].getsockname()[1]
        mcp, _ = build_server(Config(host="127.0.0.1", port=port, token="wrong"))
        msg = await _error(mcp, "game_status")
        srv.close()
        return msg
    assert "unauthorized" in run(main())


def test_wait_for_events_blocks_until_event_and_advances_cursor():
    async def main():
        game, srv, mcp, session = await _start()
        await _call(mcp, "observe")  # establishes cursor
        async def later():
            await asyncio.sleep(0.2)
            await game.emit("player_chat", text="Lydia, what do you think?")
        asyncio.create_task(later())
        first = json.loads(_text(await _call(mcp, "wait_for_events", timeout_s=5)))
        second = json.loads(_text(await _call(mcp, "wait_for_events", timeout_s=0.2)))
        srv.close()
        return first, second
    first, second = run(main())
    assert [e["type"] for e in first["events"]] == ["player_chat"]
    assert second["events"] == []


def test_say_sends_audio_and_waits_for_completion(tmp_path):
    wav = tmp_path / "line.wav"
    pcm = struct.pack("<" + "h" * 2205, *([1000, -1000] * 1102 + [0]))
    header = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
              struct.pack("<IHHIIHH", 16, 1, 1, 22050, 44100, 2, 16) + b"data" + struct.pack("<I", len(pcm)))
    wav.write_bytes(header + pcm)

    async def main():
        game, srv, mcp, _ = await _start()
        out = json.loads(_text(await _call(mcp, "say", text="For Whiterun!", audio_path=str(wav), wait=True)))
        sent = [p for m, p in game.log if m == "say"][0]
        srv.close()
        return out, sent
    out, sent = run(main())
    assert out["finished"] is True and out["lipsync"] is True
    assert "audio_b64" in sent and sent["text"] == "For Whiterun!"


def test_say_without_tts_is_subtitle_only():
    async def main():
        game, srv, mcp, _ = await _start()
        out = json.loads(_text(await _call(mcp, "say", text="Hmm.")))
        sent = [p for m, p in game.log if m == "say"][0]
        srv.close()
        return out, sent
    out, sent = run(main())
    assert "audio_b64" not in sent and out["lipsync"] is False


def test_see_returns_image_content():
    async def main():
        _, srv, mcp, _ = await _start()
        res = await _call(mcp, "see", perspective="agent")
        srv.close()
        return res
    res = run(main())
    assert not res.is_error
    kinds = [c.type for c in res.content]
    assert "image" in kinds
    meta = json.loads([c.text for c in res.content if c.type == "text"][0])
    assert meta["pov_applied"] is True


def test_move_to_wait_and_mod_actions():
    async def main():
        _, srv, mcp, _ = await _start()
        moved = json.loads(_text(await _call(mcp, "move_to", target="0x0001B1DB", wait=True, timeout_s=5)))
        acts = json.loads(_text(await _call(mcp, "list_mod_actions")))
        done = json.loads(_text(await _call(mcp, "perform_mod_action", name="dance", timeout_s=5)))
        bad = await _error(mcp, "move_to")
        srv.close()
        return moved, acts, done, bad
    moved, acts, done, bad = run(main())
    assert moved["finished"]["ok"] is True
    assert acts["actions"][0]["name"] == "dance"
    assert done["finished"]["ok"] is True
    assert "bad_request" in bad


def test_debug_tools_opt_in():
    async def main():
        _, srv, mcp, _ = await _start(debug_tools=True)
        names = {t.name for t in await mcp.list_tools()}
        msg = await _error(mcp, "console_command", command="tgm")
        srv.close()
        return names, msg
    names, msg = run(main())
    assert "console_command" in names
    assert "disabled" in msg  # the (mock) plugin still refuses unless its INI allows it


def test_unreachable_game_gives_helpful_error():
    async def main():
        mcp, _ = build_server(Config(host="127.0.0.1", port=1, token="", connect_timeout=1))
        return await _error(mcp, "observe")
    assert "Skyrim" in run(main())


def test_wav_info_and_streamed_header_fix():
    pcm = b"\x00\x00" * 100
    hdr = (b"RIFF" + struct.pack("<I", 0xFFFFFFFF) + b"WAVEfmt " +
           struct.pack("<IHHIIHH", 16, 1, 1, 24000, 48000, 2, 16) + b"data" + struct.pack("<I", 0xFFFFFFFF))
    fixed = tts._fix_streamed_wav(hdr + pcm)
    info = tts.wav_info(fixed)
    assert info and info["rate"] == 24000 and abs(info["duration_s"] - 100 / 24000) < 1e-9
    assert tts.wav_info(b"ID3" + b"\x00" * 100) is None


def test_config_env_layering(tmp_path):
    f = tmp_path / "c.json"
    f.write_text(json.dumps({"agent": "serana", "tts": "openai", "port": 5000}))
    cfg = Config.load({"ANIMUNCULI_CONFIG": str(f), "ANIMUNCULI_PORT": "6000", "ANIMUNCULI_DEBUG_TOOLS": "yes"})
    assert (cfg.agent, cfg.tts, cfg.port, cfg.debug_tools) == ("serana", "openai", 6000, True)
