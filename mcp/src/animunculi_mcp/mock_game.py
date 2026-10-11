"""A fake Animunculi game endpoint that speaks the real wire protocol.

Use it to develop agents or the MCP server without Skyrim running:

    uv run animunculi-mock-game --port 47700 --token test
    ANIMUNCULI_PORT=47700 ANIMUNCULI_TOKEN=test uv run animunculi-mcp

It simulates a companion (Lydia) standing with the player outside Whiterun. It answers the protocol
methods with plausible data and occasionally emits `heard` events.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import json
import struct
import time
import zlib
from typing import Any

from . import PROTOCOL_VERSION


def _png(width: int, height: int) -> bytes:
    """A small gradient PNG (stdlib only), used as the mock camera image."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes((40 + x * 150 // width, 90 + y * 120 // height, 160))
    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b""))


class MockGame:
    def __init__(self, token: str = "", chatter: float = 0.0):
        self.token = token
        self.chatter = chatter
        self.seq = 1
        self.events: list[dict[str, Any]] = []
        self.cond = asyncio.Condition()
        self.bound: dict[str, str] = {"default": "0x000A2C94"}
        self.speech_id = 0
        self.action_id = 0
        self.waiting = False
        self.log: list[tuple[str, dict]] = []  # (method, params) for tests

    # -------------------------------------------------------------- events
    async def emit(self, type_: str, **data: Any) -> None:
        async with self.cond:
            ev = {"seq": self.seq, "type": type_, "t": int(time.time() * 1000), "game_hours": 14.2, **data}
            self.seq += 1
            self.events.append(ev)
            del self.events[:-512]
            self.cond.notify_all()

    def _select(self, since: int, types: list[str] | None, limit: int) -> tuple[list, int]:
        picked = [e for e in self.events if e["seq"] >= since and (not types or e["type"] in types)]
        return picked[:limit], (picked[limit - 1]["seq"] + 1 if len(picked) > limit else self.seq)

    # -------------------------------------------------------------- world
    def _lydia(self) -> dict:
        return {"id": "0x000A2C94", "name": "Lydia", "race": "Nord", "level": 14, "sex": "female",
                "health": 1.0, "magicka": 1.0, "stamina": 0.9, "in_combat": False,
                "weapon_drawn": False, "sneaking": False, "waiting": self.waiting,
                "position": [22012.0, -8950.0, -3140.0], "heading_deg": 12.0,
                "equipped": {"right": "Steel Sword", "left": "Steel Shield", "shout": None},
                "speaking": False}

    def _nearby(self) -> list[dict]:
        return [
            {"id": "0x00000014", "kind": "actor", "name": "Player", "dist_m": 2.1, "bearing": -35,
             "dz_m": 0.0, "hostile": False, "is_player": True, "health": 0.8},
            {"id": "0x0001A67C", "kind": "actor", "name": "Whiterun Guard", "race": "Nord",
             "dist_m": 14.5, "bearing": 60, "dz_m": 1.2, "hostile": False, "health": 1.0, "level": 18},
            {"id": "0x000B2D11", "kind": "container", "name": "Barrel", "dist_m": 6.3, "bearing": 110,
             "dz_m": 0.0, "locked": False},
            {"id": "0x0001B1DB", "kind": "door", "name": "Whiterun", "dist_m": 22.0, "bearing": 5,
             "dz_m": 3.0, "destination": "Whiterun", "locked": False},
            {"id": "0xFF0012AB", "kind": "item", "name": "Iron Dagger", "dist_m": 3.4, "bearing": -120,
             "dz_m": -0.2, "value": 10, "weight": 2.0},
            {"id": "0xFF00A110", "kind": "actor", "name": "Lydia's Horse", "race": "Horse", "dist_m": 9.0,
             "bearing": 150, "dz_m": 0.0, "health": 1.0, "rideable": True, "owner": "Lydia", "yours": True},
        ]

    async def handle(self, method: str, p: dict[str, Any]) -> dict[str, Any]:
        self.log.append((method, p))
        agent = p.get("agent", "default")
        if method in ("observe", "nearby", "inspect", "inventory", "say", "move_to", "follow",
                      "wait_here", "activate", "pick_up", "give", "use_item", "unequip", "cast",
                      "attack", "stop_combat", "look_at", "emote", "draw_weapon", "open_inventory",
                      "perform", "stop_speaking", "sit", "stand", "learn_spell", "mount", "dismount") and agent not in self.bound:
            raise ProtocolError("not_bound", f"agent '{agent}' has no body")
        if method == "ping":
            return {"pong": True}
        if method == "status":
            return {"in_game": True, "paused": False, "menus": [], "game_time": "Sundas, 17th of Last Seed, 4E 201, 2:12 PM",
                    "player": {"name": "Prisoner", "level": 9},
                    "agents": [{"agent": a, "bound": True, "id": i, "name": "Lydia", "loaded": True}
                               for a, i in self.bound.items()],
                    "features": {"voice": True, "vision": True, "pov": True, "console": False, "chat_ui": True}}
        if method == "agents":
            return {"agents": [{"agent": a, "bound": True, "id": i, "name": "Lydia", "loaded": True}
                               for a, i in self.bound.items()]}
        if method == "bind":
            self.bound[agent] = "0x000A2C94"
            await self.emit("agent_bound", agent=agent, id="0x000A2C94", name="Lydia")
            return {"agent": agent, "id": "0x000A2C94", "name": "Lydia"}
        if method == "unbind":
            self.bound.pop(agent, None)
            return {}
        if method == "observe":
            near = self._nearby()
            return {"time": {"text": "Sundas, 17th of Last Seed, 4E 201, 2:12 PM", "hour": 14.2},
                    "location": {"location": "Whiterun Hold", "worldspace": "Skyrim", "interior": False,
                                 "weather": "clear", "types": ["Hold"],
                                 "near": {"name": "Whiterun", "type": "city", "dist_m": 380, "direction": "N",
                                          "text": "near Whiterun (380 m N)"}},
                    "self": self._lydia(),
                    "player": {"id": "0x00000014", "name": "Prisoner", "level": 9, "health": 0.8,
                               "in_combat": False, "dist_m": 2.1, "bearing": -35, "sneaking": False},
                    "actors": [n for n in near if n["kind"] == "actor"][: p.get("max_actors", 12)],
                    "objects": [n for n in near if n["kind"] != "actor"][: p.get("max_objects", 12)],
                    "speaking": False}
        if method == "nearby":
            kinds = p.get("kinds")
            return {"items": [n for n in self._nearby() if not kinds or n["kind"] in kinds][: p.get("limit", 25)]}
        if method == "inspect":
            for n in self._nearby():
                if p.get("target") in (n["id"], n["name"]):
                    return dict(n, description="mock")
            if p.get("target") in ("self", "Lydia"):
                return self._lydia()
            raise ProtocolError("not_found", f"no target '{p.get('target')}' nearby")
        if method == "inventory":
            return {"owner": "Lydia", "gold": 25, "items": [
                {"id": "0x00013989", "name": "Steel Sword", "kind": "weapon", "count": 1, "equipped": True},
                {"id": "0x0003B5A7", "name": "Potion of Minor Healing", "kind": "potion", "count": 3}]}
        if method == "quests":
            return {"quests": [{"id": "0x0002610C", "name": "Bleak Falls Barrow", "type": "main",
                                "stage": 10, "objectives": ["Retrieve the Dragonstone"]}]}
        if method == "crosshair":
            return {"target": None}
        if method in ("events", "wait_events"):
            since = int(p.get("since", 0))
            types = p.get("types") or None
            limit = int(p.get("limit", 100))
            if method == "wait_events":
                deadline = time.monotonic() + min(int(p.get("timeout_ms", 0)), 120000) / 1000
                async with self.cond:
                    while not self._select(since, types, limit)[0]:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            break
                        try:
                            await asyncio.wait_for(self.cond.wait(), remaining)
                        except asyncio.TimeoutError:
                            break
            events, nxt = self._select(since, types, limit)
            if not events and types:
                nxt = self.seq  # skipped over non-matching events
            return {"events": events, "next_seq": nxt, "dropped": 0}
        if method == "say":
            self.speech_id += 1
            sid = self.speech_id
            has_audio = bool(p.get("audio_b64") or p.get("audio_path"))
            dur = 0.4 if has_audio else max(1.5, len(p.get("text", "")) * 0.06)
            await self.emit("speech_started", speech_id=sid, agent=agent, speaker="Lydia", text=p.get("text"),
                            duration_ms=int(dur * 1000))
            async def finish():
                await asyncio.sleep(dur)
                await self.emit("speech_finished", speech_id=sid, agent=agent)
            asyncio.create_task(finish())
            return {"speech_id": sid, "duration_ms": int(dur * 1000), "lipsync": has_audio}
        if method == "capture":
            data = _png(64, 36)
            res = {"mime": "image/png", "data_b64": base64.b64encode(data).decode(), "width": 64,
                   "height": 36, "perspective": p.get("perspective", "player")}
            if p.get("perspective") == "agent":
                res["pov_applied"] = True
            if p.get("save"):
                res["saved_to"] = "C:/Users/you/Documents/My Games/Skyrim Special Edition/Screenshots/Animunculi/mock.png"
            return res
        if method in ("move_to", "perform"):
            if method == "perform" and p.get("name") != "dance":
                raise ProtocolError("not_found", f"no mod action '{p.get('name')}'")
            self.action_id += 1
            aid = self.action_id
            async def arrive():
                await asyncio.sleep(0.3)
                await self.emit("action_finished", action_id=aid, agent=agent, ok=True, action=method)
            asyncio.create_task(arrive())
            return {"action_id": aid}
        if method == "actions":
            return {"actions": [{"name": "dance", "description": "Dance a little jig", "params": "{}"}]}
        if method == "wait_here":
            self.waiting = True
            return {"ok": True}
        if method == "follow":
            self.waiting = False
            return {"ok": True}
        if method == "sit":
            self.action_id += 1
            aid = self.action_id
            async def seated():
                await asyncio.sleep(0.2)
                await self.emit("action_finished", action_id=aid, agent=agent, action="sit", ok=True, sitting=True, seat="Bench")
            asyncio.create_task(seated())
            return {"action_id": aid, "seat": {"id": "0x000B1A2C", "kind": "furniture", "name": "Bench", "dist_m": 3.1}}
        if method == "stand":
            return {"ok": True}
        if method == "mount" or (method == "activate" and p.get("target") in ("0xFF00A110", "Lydia's Horse")):
            if self.__dict__.get("riding"):
                return {"ok": True, "already_riding": "Lydia's Horse"}
            self.action_id += 1
            aid = self.action_id
            action = method
            async def mounted():
                await asyncio.sleep(0.2)
                self.__dict__["riding"] = True
                await self.emit("action_finished", action_id=aid, agent=agent, action=action, ok=True,
                                mounted=True, target="Lydia's Horse", kind="actor")
            asyncio.create_task(mounted())
            return {"action_id": aid, "approaching": True, "distance_m": 9.0,
                    "horse": {"id": "0xFF00A110", "name": "Lydia's Horse", "yours": True}}
        if method == "dismount":
            if not self.__dict__.get("riding"):
                return {"ok": False, "note": "you are not riding"}
            self.__dict__["riding"] = False
            return {"ok": True, "horse": "Lydia's Horse"}
        if method == "gestures":
            return {"gestures": [{"name": "wave", "what": "wave hello", "pose": False},
                                 {"name": "sit_ground", "what": "sit cross-legged on the ground (pose)", "pose": True}]}
        if method == "spells":
            return {"spells": [{"id": "0x00012FCD", "name": "Flames", "kind": "spell", "magicka_cost": 14, "delivery": "aimed",
                                "casting": "concentration", "school": "destruction", "equipped": "right"},
                               {"id": "0x0002F3B8", "name": "Healing Hands", "kind": "spell", "magicka_cost": 49,
                                "delivery": "aimed", "casting": "concentration", "school": "restoration", "heals_others": True}],
                    "magicka": 120, "magicka_max": 150}
        if method == "learn_spell":
            if "healing" not in p.get("tome", "").lower():
                raise ProtocolError("failed", f"{p.get('tome')} is not a spell tome")
            return {"ok": True, "learned": {"name": "Healing Hands", "magicka_cost": 49, "heals_others": True}}
        if method == "debug_emit":  # mock only: lets out-of-process tests inject world events
            await self.emit(p.get("type", "custom"), **(p.get("data") or {}))
            return {"ok": True}
        if method == "presence":
            self.__dict__["thinking"] = bool(p.get("thinking"))
            return {"ok": True}
        if method == "console":
            raise ProtocolError("disabled", "console commands are disabled ([Debug] AllowConsole)")
        if method in ("activate", "pick_up", "give", "use_item", "unequip", "cast", "attack",
                      "stop_combat", "look_at", "emote", "draw_weapon", "open_inventory", "notify",
                      "stop_speaking"):
            return {"ok": True}
        if method == "read":
            if p.get("target") in (None, "", "open"):
                raise ProtocolError("bad_request", "name a book, or open one")
            text = "The Lusty Argonian Maid\n\nLifts-Her-Tail: Certainly not, kind sir! " * 3
            off, n = int(p.get("offset", 0)), int(p.get("max_chars", 6000))
            res = {"id": "0x0001AFDA", "title": "The Lusty Argonian Maid", "kind": "book", "source": "nearby",
                   "text": text[off:off + n], "total_chars": len(text), "offset": off}
            if off + n < len(text):
                res["next_offset"] = off + n
            return res
        if method == "map":
            return {"here": {"location": "Whiterun Hold", "hold": "Whiterun Hold", "map": "Skyrim", "interior": False},
                    "places": [{"name": "Whiterun", "type": "city", "dist_m": 380, "direction": "N", "bearing": 5, "discovered": True},
                               {"name": "Riverwood", "type": "town", "dist_km": 4.1, "direction": "SW", "bearing": -150, "discovered": True}],
                    "quest_targets": [{"quest": "Bleak Falls Barrow", "objective": "Retrieve the Dragonstone",
                                       "target": "Dragonstone", "where": "Bleak Falls Sanctum", "dist_km": 7.9, "direction": "SW"}]}
        if method == "locate":
            if "black" in p.get("name", "").lower():
                return {"matches": [{"name": "Blackreach", "type": "region (separate map)",
                                     "entrances": ["Alftand Animonculory", "Mzinchaleft Depths", "Raldbthar Deep Market"]}]}
            raise ProtocolError("not_found", f"no place called '{p.get('name')}' on any map")
        if method == "dialogue_options":
            return {"open": True, "with": {"id": "0x0001B071", "name": "Urag gro-Shub"},
                    "topics": ["What can you tell me about the Arcanaeum?", "Goodbye."], "last_chosen": None}
        if method in ("set_combat", "set_follow_style", "orders"):
            orders = self.__dict__.setdefault("orders", {"combat": {"style": "default", "restraint": False, "medic": False,
                                                                    "medic_threshold": 0.4},
                                                         "follow": {"mode": "relaxed", "distance_m": 2.0, "side": "left",
                                                                    "remount": True}})
            if method == "set_combat":
                if p.get("style") and p["style"] not in ("default", "ranged", "magic", "spellsword", "melee", "defensive", "berserker"):
                    raise ProtocolError("bad_request", f"unknown style '{p['style']}'")
                for k in ("style", "restraint", "medic", "medic_threshold"):
                    if p.get(k) is not None:
                        orders["combat"][k] = p[k]
            if method == "set_follow_style":
                f = orders["follow"]
                for k_in, k_out in (("mode", "mode"), ("distance_m", "distance_m"), ("side", "side"), ("remount", "remount")):
                    if p.get(k_in) is not None:
                        f[k_out] = p[k_in]
            return orders
        raise ProtocolError("unknown_method", method)

    # -------------------------------------------------------------- transport
    async def serve_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        authed = False
        wlock = asyncio.Lock()

        async def reply(msg: dict) -> None:
            async with wlock:
                writer.write(json.dumps(msg).encode() + b"\n")
                await writer.drain()

        async def run(req: dict) -> None:
            try:
                result = await self.handle(req.get("method", ""), req.get("params") or {})
                await reply({"id": req.get("id"), "result": result})
            except ProtocolError as exc:
                await reply({"id": req.get("id"), "error": {"code": exc.code, "message": exc.message}})

        try:
            while line := await reader.readline():
                req = json.loads(line)
                if not authed:
                    params = req.get("params") or {}
                    if req.get("method") != "hello" or (self.token and params.get("token") != self.token):
                        await reply({"id": req.get("id"), "error": {"code": "unauthorized", "message": "bad token"}})
                        break
                    authed = True
                    await reply({"id": req.get("id"), "result": {
                        "plugin": "Animunculi", "plugin_version": "0.1.0-mock", "game_version": "1.6.1170.0",
                        "runtime": "AE", "protocol": PROTOCOL_VERSION, "event_seq": self.seq}})
                    continue
                asyncio.create_task(run(req))
        except (ConnectionError, ValueError):
            pass
        finally:
            writer.close()

    async def chatter_loop(self) -> None:
        lines = [("Whiterun Guard", "I used to be an adventurer like you..."),
                 ("Whiterun Guard", "Everything all right?")]
        i = 0
        while self.chatter > 0:
            await asyncio.sleep(self.chatter)
            who, text = lines[i % len(lines)]
            i += 1
            await self.emit("heard", speaker={"id": "0x0001A67C", "name": who}, text=text, dist_m=14.5)


class ProtocolError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code, self.message = code, message


async def serve(host: str, port: int, token: str, chatter: float) -> None:
    game = MockGame(token, chatter)
    server = await asyncio.start_server(game.serve_client, host, port, limit=48 * 1024 * 1024)
    print(f"mock Animunculi game on {host}:{port} (token={'set' if token else 'none'})", flush=True)
    asyncio.create_task(game.chatter_loop())
    async with server:
        await server.serve_forever()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=47700)
    ap.add_argument("--token", default="")
    ap.add_argument("--chatter", type=float, default=20.0, help="seconds between overheard lines (0=off)")
    args = ap.parse_args()
    try:
        asyncio.run(serve(args.host, args.port, args.token, args.chatter))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
