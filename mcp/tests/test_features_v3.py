"""Second playtest list: landmarks, sitting, gestures, spells, and waits that fit in a tool call."""
from __future__ import annotations

import asyncio
import json
import time

import pytest
from mcp.server.mcpserver.exceptions import ToolError

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


def test_new_body_tools_are_listed():
    async def main():
        _, srv, mcp = await _start()
        names = {t.name for t in await mcp.list_tools()}
        srv.close()
        return names
    assert {"sit", "stand_up", "gestures", "spells", "learn_spell"} <= asyncio.run(main())


def test_observe_names_the_nearest_landmark():
    async def main():
        _, srv, mcp = await _start()
        obs = await _j(mcp, "observe")
        srv.close()
        return obs
    assert asyncio.run(main())["location"]["near"]["text"] == "near Whiterun (380 m N)"


def test_sit_gestures_spells_and_learning():
    async def main():
        game, srv, mcp = await _start()
        sat = await _j(mcp, "sit", target="crate", pose="sit")
        done = await _j(mcp, "wait_for_events", timeout_s=3, types=["action_finished"])
        up = await _j(mcp, "stand_up")
        g = await _j(mcp, "gestures")
        sp = await _j(mcp, "spells")
        learned = await _j(mcp, "learn_spell", tome="Spell Tome: Healing Hands")
        with pytest.raises(ToolError):
            await mcp.call_tool("learn_spell", {"tome": "The Lusty Argonian Maid"})
        sent = {m: p for m, p in game.log}
        srv.close()
        return sat, done, up, g, sp, learned, sent
    sat, done, up, g, sp, learned, sent = asyncio.run(main())
    assert sat["action_id"] and sent["sit"]["target"] == "crate" and sent["sit"]["pose"] == "sit"
    assert done["events"][0]["sitting"] is True
    assert up["ok"] is True
    assert any(x["pose"] for x in g["gestures"])
    assert any(x.get("heals_others") for x in sp["spells"]) and sp["magicka_max"] == 150
    assert learned["learned"]["name"] == "Healing Hands"


def test_waits_stay_under_the_host_tool_timeout():
    async def main():
        _, srv, mcp = await _start(max_wait_s=0.6)
        await _j(mcp, "observe")
        t0 = time.monotonic()
        res = await _j(mcp, "wait_for_events", timeout_s=999)
        elapsed = time.monotonic() - t0
        srv.close()
        return res, elapsed
    res, elapsed = asyncio.run(main())
    assert res["events"] == [] and elapsed < 3.0
