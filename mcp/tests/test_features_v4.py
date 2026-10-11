"""Third playtest list: horses, partial standing-order updates, and a clearer `see`."""
from __future__ import annotations

import asyncio
import json

from animunculi_mcp.config import Config
from animunculi_mcp.mock_game import MockGame
from animunculi_mcp.server import build_server


async def _start(**cfg_kw):
    game = MockGame(token="t", chatter=0)
    srv = await asyncio.start_server(game.serve_client, "127.0.0.1", 0)
    port = srv.sockets[0].getsockname()[1]
    mcp, _ = build_server(Config(host="127.0.0.1", port=port, token="t", **cfg_kw))
    return game, srv, mcp


async def _j(mcp, name, **args):
    res = await mcp.call_tool(name, args)
    return json.loads("".join(c.text for c in res.content if c.type == "text"))


def test_horse_tools_are_listed():
    async def main():
        _, srv, mcp = await _start()
        names = {t.name for t in await mcp.list_tools()}
        srv.close()
        return names
    assert {"mount", "dismount"} <= asyncio.run(main())


def test_mount_waits_for_the_climb_and_dismount_reports():
    async def main():
        game, srv, mcp = await _start()
        on = await _j(mcp, "mount", wait=True, timeout_s=5)
        again = await _j(mcp, "mount")
        off = await _j(mcp, "dismount")
        off_again = await _j(mcp, "dismount")
        sent = [m for m, _ in game.log]
        srv.close()
        return on, again, off, off_again, sent
    on, again, off, off_again, sent = asyncio.run(main())
    assert on["finished"]["mounted"] is True and on["finished"]["action"] == "mount"
    assert again["already_riding"] == "Lydia's Horse"
    assert off["ok"] is True and off_again["ok"] is False
    assert "mount" in sent and "dismount" in sent


def test_activate_passes_approach_and_can_wait():
    async def main():
        game, srv, mcp = await _start()
        res = await _j(mcp, "activate", target="0xFF00A110", wait=True, timeout_s=5)
        plain = await _j(mcp, "activate", target="0x000B2D11", approach=False)
        sent = [p for m, p in game.log if m == "activate"]
        srv.close()
        return res, plain, sent
    res, plain, sent = asyncio.run(main())
    assert res["approaching"] is True and res["finished"]["ok"] is True
    assert plain["ok"] is True
    assert sent[0]["approach"] is True and sent[1]["approach"] is False


def test_set_follow_style_changes_only_what_is_given():
    async def main():
        game, srv, mcp = await _start()
        await _j(mcp, "set_follow_style", mode="close", side="right", distance_m=3.0)
        res = await _j(mcp, "set_follow_style", remount=False)
        sent = [p for m, p in game.log if m == "set_follow_style"]
        srv.close()
        return res, sent
    res, sent = asyncio.run(main())
    assert res["follow"] == {"mode": "close", "distance_m": 3.0, "side": "right", "remount": False}
    assert "mode" not in sent[1] and sent[1]["remount"] is False  # absent = keep the current value


def test_see_says_whose_view_it_is():
    async def main():
        _, srv, mcp = await _start()
        player = await mcp.call_tool("see", {})
        agent = await mcp.call_tool("see", {"perspective": "agent"})
        tools = {t.name: t for t in await mcp.list_tools()}
        srv.close()
        return player, agent, tools["see"]
    player, agent, see = asyncio.run(main())
    meta = lambda r: json.loads(next(c.text for c in r.content if c.type == "text"))
    assert "player's screen" in meta(player)["note"]
    assert meta(agent)["pov_applied"] is True and "note" not in meta(agent)
    assert see.input_schema["properties"]["perspective"]["enum"] == ["player", "agent"]


def test_look_at_is_timed():
    async def main():
        game, srv, mcp = await _start()
        await _j(mcp, "look_at", target="player")
        await _j(mcp, "look_at", target="0x0001A67C", duration_s=0)
        sent = [p for m, p in game.log if m == "look_at"]
        srv.close()
        return sent
    sent = asyncio.run(main())
    assert sent[0]["duration_s"] == 15.0 and sent[1]["duration_s"] == 0
