"""Wishlist features: books, map, dialogue topics, standing orders, keepsakes, waking on what matters."""
from __future__ import annotations

import asyncio
import json

import pytest
from mcp.server.mcpserver.exceptions import ToolError

from animunculi_mcp.config import Config
from animunculi_mcp.importance import importance, split
from animunculi_mcp.mock_game import MockGame
from animunculi_mcp.server import build_server
from animunculi_mcp.watch import watch


async def _start():
    game = MockGame(token="t")
    srv = await asyncio.start_server(game.serve_client, "127.0.0.1", 0)
    port = srv.sockets[0].getsockname()[1]
    cfg = Config(host="127.0.0.1", port=port, token="t")
    mcp, session = build_server(cfg)
    return game, srv, mcp, cfg


async def _j(mcp, name, **args):
    res = await mcp.call_tool(name, args)
    return json.loads("".join(c.text for c in res.content if c.type == "text"))


def test_new_tools_are_listed():
    async def main():
        _, srv, mcp, _ = await _start()
        names = {t.name for t in await mcp.list_tools()}
        srv.close()
        return names
    names = asyncio.run(main())
    for n in ("read", "map", "locate", "dialogue_options", "set_combat", "set_follow_style"):
        assert n in names


def test_read_pages_through_a_book():
    async def main():
        game, srv, mcp, _ = await _start()
        first = await _j(mcp, "read", book="0x0001AFDA", max_chars=40)
        second = await _j(mcp, "read", book="0x0001AFDA", offset=first["next_offset"], max_chars=40)
        sent = [p for m, p in game.log if m == "read"]
        srv.close()
        return first, second, sent
    first, second, sent = asyncio.run(main())
    assert first["title"] == "The Lusty Argonian Maid" and len(first["text"]) == 40
    assert second["offset"] == 40
    assert sent[0]["target"] == "0x0001AFDA"


def test_map_locate_and_dialogue():
    async def main():
        _, srv, mcp, _ = await _start()
        m = await _j(mcp, "map")
        loc = await _j(mcp, "locate", place="Blackreach")
        with pytest.raises(ToolError):
            await mcp.call_tool("locate", {"place": "Atlantis"})
        d = await _j(mcp, "dialogue_options")
        srv.close()
        return m, loc, d
    m, loc, d = asyncio.run(main())
    assert m["places"][0]["name"] == "Whiterun" and m["quest_targets"][0]["quest"] == "Bleak Falls Barrow"
    assert "Alftand Animonculory" in loc["matches"][0]["entrances"]
    assert d["with"]["name"] == "Urag gro-Shub" and d["topics"]


def test_standing_orders_round_trip_and_show_in_status():
    async def main():
        _, srv, mcp, _ = await _start()
        c = await _j(mcp, "set_combat", style="ranged", restraint=True, medic=True)
        f = await _j(mcp, "set_follow_style", mode="close", distance_m=1.5, side="right")
        with pytest.raises(ToolError):
            await mcp.call_tool("set_combat", {"style": "ninja"})
        st = await _j(mcp, "game_status")
        srv.close()
        return c, f, st
    c, f, st = asyncio.run(main())
    assert c["combat"] == {"style": "ranged", "restraint": True, "medic": True, "medic_threshold": 0.4}
    assert f["follow"] == {"mode": "close", "distance_m": 1.5, "side": "right", "remount": True}
    assert st["orders"]["combat"]["style"] == "ranged"


def test_see_keepsake_and_hands():
    async def main():
        game, srv, mcp, _ = await _start()
        res = await mcp.call_tool("see", {"perspective": "agent", "save": True, "caption": "Arcanaeum at dusk"})
        await _j(mcp, "use_item", item="Healing", hand="left")
        sent = {m: p for m, p in game.log}
        srv.close()
        return res, sent
    res, sent = asyncio.run(main())
    meta = json.loads([c.text for c in res.content if c.type == "text"][0])
    assert meta["saved_to"].endswith(".png")
    assert sent["capture"]["save"] is True and sent["capture"]["caption"] == "Arcanaeum at dusk"
    assert sent["use_item"]["hand"] == "left"


# ---------------------------------------------------------------------- importance
def test_importance_rules():
    me = "default"
    assert importance({"type": "player_chat", "text": "hi"}, me)
    assert importance({"type": "player_chat", "agent": "serana"}, me) is None
    assert importance({"type": "dialogue_started", "with": {"agent": me}}, me)
    assert importance({"type": "dialogue_started", "with": {"name": "Urag"}}, me) is None
    assert importance({"type": "combat", "state": "combat"}, me)
    assert importance({"type": "combat", "state": "none"}, me) is None
    assert importance({"type": "location_changed", "to": "Arcanaeum"}, me) == "arrived at Arcanaeum"
    assert importance({"type": "item_received", "agent": me, "how": "given_by_player", "item": "Apple"}, me)
    assert importance({"type": "item_received", "agent": me, "how": "spawned"}, me) is None
    assert importance({"type": "health_low", "who": {"is_player": True}}, me)
    assert importance({"type": "heard", "text": "..."}, me) is None
    assert importance({"type": "hit"}, me) is None
    important, rest = split([{"type": "heard"}, {"type": "heard"}, {"type": "player_chat", "text": "x"}], me)
    assert [e["type"] for e in important] == ["player_chat"] and important[0]["why"]
    assert rest == {"heard": 2}


def test_wait_for_events_important_only_sleeps_through_noise():
    async def main():
        game, srv, mcp, _ = await _start()
        await _j(mcp, "observe")  # establish the cursor
        async def later():
            await asyncio.sleep(0.1)
            await game.emit("heard", speaker={"name": "Guard"}, text="Halt")
            await asyncio.sleep(0.2)
            await game.emit("hit", target={"name": "Wolf"})
            await asyncio.sleep(0.2)
            await game.emit("player_chat", text="Did you see that?")
        asyncio.create_task(later())
        res = await _j(mcp, "wait_for_events", timeout_s=5, important_only=True)
        quiet = await _j(mcp, "wait_for_events", timeout_s=0.3, important_only=True)
        srv.close()
        return res, quiet
    res, quiet = asyncio.run(main())
    assert [e["type"] for e in res["events"]] == ["player_chat"] and res["events"][0]["why"]
    assert res["also_happened"] == {"heard": 1, "hit": 1}
    assert quiet["events"] == [] and "nothing important" in quiet["note"]


def test_watcher_wakes_on_important_event_and_times_out():
    async def main():
        game, srv, _, cfg = await _start()
        lines: list[str] = []
        async def later():
            await asyncio.sleep(0.2)
            await game.emit("heard", text="noise")
            await game.emit("combat", state="combat", actor={"name": "Bandit"})
        asyncio.create_task(later())
        code = await watch(cfg, once=True, timeout_s=5, emit=lines.append, gather_s=0.2)
        timeout_lines: list[str] = []
        timeout_code = await watch(cfg, once=True, timeout_s=0.5, emit=timeout_lines.append, gather_s=0.1)
        srv.close()
        return code, lines, timeout_code, timeout_lines
    code, lines, timeout_code, timeout_lines = asyncio.run(main())
    assert code == 0
    out = json.loads(lines[0])
    assert out["events"][0]["type"] == "combat" and out["also"] == {"heard": 1}
    assert timeout_code == 3 and json.loads(timeout_lines[0]) == {"timeout": True}


def test_watcher_waits_for_the_game_to_come_up():
    # nothing listening: it keeps retrying quietly and reports a timeout
    async def main():
        lines: list[str] = []
        code = await watch(Config(host="127.0.0.1", port=1, token="", connect_timeout=0.2), once=True,
                           timeout_s=0.6, emit=lines.append)
        return code, lines
    code, lines = asyncio.run(main())
    assert code == 3 and json.loads(lines[-1]) == {"timeout": True}
