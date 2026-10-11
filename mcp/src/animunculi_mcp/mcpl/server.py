"""animunculi-mcpl: Animunculi as an MCPL 0.5 server (MCP superset) over stdio.

Plain MCP hosts see the same tools as `animunculi-mcp`. MCPL hosts additionally get:

  skyrim.events           push/event for what matters (being spoken to, fights, arrivals, loot,
                          someone badly hurt or down, being left behind), tagged so the host's
                          wake policy decides what wakes the model
  skyrim.chat             channel `skyrim:<agent>`: the player's typed lines arrive as
                          channels/incoming (chat:dm); what the host publishes to it is spoken
                          aloud in-game by the body (voice, lip sync, subtitles)
  skyrim.chat.overhear    once the host opens that channel, nearby NPC speech and the player's
                          conversations with others arrive too (chat:ambient)
  skyrim.presence         context/beforeInference (inject-only, never reads the user's message):
                          one line of body status before every turn
  skyrim.thinking         inference/lifecycle: shows the player when the agent is thinking

Nothing capability-dependent happens until the host's initial featureSets/update Request has been
answered (SPEC §5.3), and every outbound use is checked against the current grant.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import sys
import threading
import time
from datetime import datetime, timezone
from typing import Any

from mcp.server.mcpserver.exceptions import ToolError

from .. import __version__
from ..bridge import Bridge, BridgeError
from ..config import Config
from ..importance import importance
from ..server import build_server
from . import policy

log = logging.getLogger("animunculi.mcpl")

SUPPORTED_PROTOCOLS = ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25")
ERR_METHOD_NOT_FOUND = -32601
ERR_INVALID_PARAMS = -32602
ERR_INTERNAL = -32603
ERR_CAPABILITY_DENIED = -32002
ERR_UNKNOWN_CHANNEL = -32023

KIND_TAGS: dict[str, tuple[str, str]] = {  # event type -> (tag, priority)
    "player_chat": ("animunculi:spoken-to", "high"),
    "player_dialogue": ("animunculi:spoken-to", "high"),
    "dialogue_started": ("animunculi:spoken-to", "high"),
    "combat": ("animunculi:combat", "high"),
    "health_low": ("animunculi:danger", "high"),
    "bleedout": ("animunculi:danger", "high"),
    "death": ("animunculi:danger", "high"),
    "agent_unloaded": ("animunculi:left-behind", "high"),
    "location_changed": ("animunculi:arrival", "normal"),
    "item_received": ("animunculi:loot", "normal"),
    "restrained": ("animunculi:orders", "normal"),
    "level_up": ("animunculi:milestone", "normal"),
    "custom": ("animunculi:mod-event", "normal"),
}

FEATURE_SETS: dict[str, dict[str, Any]] = {
    "skyrim.events": {
        "description": "Wake the agent for what matters in Skyrim: being spoken to, fights, arriving somewhere "
                       "new, loot, someone badly hurt or down, being left behind",
        "uses": ["pushEvents"],
        "tagOntology": {
            "coreTags": ["chat:dm", "chat:addressed", "chat:from-human"],
            "tags": {
                "animunculi:spoken-to": {"desc": "the player addressed the agent", "facet": "kind"},
                "animunculi:combat": {"desc": "a fight involving the party started", "facet": "kind"},
                "animunculi:danger": {"desc": "the player or the agent is badly hurt, down or dead", "facet": "kind"},
                "animunculi:left-behind": {"desc": "the agent's body is no longer near the player", "facet": "kind"},
                "animunculi:arrival": {"desc": "the player arrived at a new location", "facet": "kind"},
                "animunculi:loot": {"desc": "the agent received an item", "facet": "kind"},
                "animunculi:orders": {"desc": "a standing order acted (e.g. restraint stopped a fight)", "facet": "kind"},
                "animunculi:milestone": {"desc": "the player levelled up", "facet": "kind"},
                "animunculi:mod-event": {"desc": "an event sent by a mod through the Papyrus API", "facet": "kind"},
            },
            "keyed": {"animunculi:priority": {"desc": "how soon a companion would react", "values": ["high", "normal", "low"],
                                              "ordered": True}},
            "defaultTreatment": [{"tagsAny": ["animunculi:priority=low"], "behavior": "mute"}],
            "open": True,
        },
    },
    "skyrim.chat": {
        "description": "The in-game conversation. The player's typed lines arrive as channel messages; text "
                       "published to the channel is spoken aloud by the agent's body (voice, lip sync, subtitles)",
        "uses": ["channels.register", "channels.incoming", "channels.publish"],
        "tagOntology": {"coreTags": ["chat:dm", "chat:addressed", "chat:from-human", "chat:private"], "open": True},
    },
    "skyrim.chat.overhear": {
        "description": "While the host holds the chat channel open, nearby NPC speech and the player's "
                       "conversations with others arrive as ambient channel messages",
        "uses": ["channels.register", "channels.lifecycle", "channels.incoming"],
        "tagOntology": {"coreTags": ["chat:ambient", "chat:from-human"], "open": True},
    },
    "skyrim.presence": {
        "description": "One line of body status (place, time, health, who is near, combat) before each turn",
        "uses": ["contextHooks.beforeInference.inject.beforeUser"],
    },
    "skyrim.thinking": {
        "description": "Show the player in-game when the agent is thinking",
        "uses": ["inferenceLifecycle"],
    },
}

CONSEQUENCES = {
    "skyrim.events": "no wake-ups; the agent must poll with wait_for_events",
    "skyrim.chat": "the player's chat only reaches the agent as push events or by polling; replies need the say tool",
    "skyrim.chat.overhear": "overheard speech stays in the pull feed (observe / wait_for_events)",
    "skyrim.presence": "no automatic status line; the agent must call observe",
    "skyrim.thinking": "no in-game thinking indicator",
}

MCPL_CAPS: dict[str, Any] = {
    "version": "0.5",
    "pushEvents": True,
    "inferenceLifecycle": True,
    "contextHooks": {"beforeInference": {"observe": False,
                                         "inject": {"system": False, "beforeUser": True, "afterUser": False}}},
    "channels": {"register": True, "lifecycle": True, "publish": True, "incoming": True},
    "featureSets": FEATURE_SETS,
}

INSTRUCTIONS = """\
You are embodied in Skyrim as one of the player's companions, through an MCPL server. The game's
follower AI moves and fights your body in real time; you provide the mind.

What this host may deliver without you asking (depending on its policy):
* Channel `skyrim:<agent>` messages: the player's typed lines. Reply by publishing to that
  channel: your reply is spoken aloud in-game by your body. Keep lines short and in character.
  If the channel is open, you also overhear nearby NPCs (ambient; usually held, not waking you).
* Push events tagged animunculi:*: fights, arrivals, loot, danger, being left behind.
* A one-line [Skyrim] status before each turn.

Tools work as in the plain MCP server: observe, map, read, see (your own eyes with
perspective="agent"), move_to, set_combat, set_follow_style, and so on. Refer to things by the ids
that observe/nearby return. Stay within what your character could plausibly do.
"""


def _iso(ms: Any) -> str:
    try:
        return datetime.fromtimestamp(int(ms) / 1000, tz=timezone.utc).isoformat().replace("+00:00", "Z")
    except (TypeError, ValueError, OSError):
        return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _name(who: Any, default: str = "someone") -> str:
    if isinstance(who, dict):
        return who.get("name") or default
    return str(who) if who else default


def render(ev: dict, why: str | None) -> str:
    """One readable line for the model (the payload the host shows in a wake)."""
    t = ev.get("type", "?")
    if t == "player_chat":
        text = f"The player says to you: “{ev.get('text', '')}”"
    elif t == "player_dialogue":
        text = f"In conversation, the player says to {_name(ev.get('to'))}: “{ev.get('text', '')}”"
    elif t == "dialogue_started":
        text = f"The player started a conversation with {_name(ev.get('with'), 'you')}"
    elif t == "combat":
        target = f" against {_name(ev.get('target'))}" if ev.get("target") else ""
        text = f"Fight: {_name(ev.get('actor'))}{target} ({ev.get('state')})"
    elif t == "health_low":
        text = f"{_name(ev.get('who'))} is badly hurt ({round(float(ev.get('health', 0)) * 100)}% health)"
    elif t == "bleedout":
        text = f"{_name(ev.get('actor'))} is down (bleeding out)"
    elif t == "death":
        killer = f", killed by {_name(ev.get('killer'))}" if ev.get("killer") else ""
        text = f"{_name(ev.get('victim'))} died{killer}"
    elif t == "location_changed":
        types = f" ({', '.join(ev['types'])})" if ev.get("types") else ""
        text = f"Arrived at {ev.get('to')}{types}" + (f", from {ev['from']}" if ev.get("from") else "")
    elif t == "item_received":
        src = f" from {_name(ev.get('from'))}" if isinstance(ev.get("from"), dict) else ""
        text = f"You received {ev.get('item')} x{ev.get('count', 1)}{src} ({ev.get('how', 'received')})"
    elif t == "agent_unloaded":
        dist = f" ({ev['dist_m']} m away)" if ev.get("dist_m") is not None else ""
        text = f"Your body was left behind{dist}; the player is out of reach"
    elif t == "restrained":
        text = f"Your restraint orders stopped you from fighting {ev.get('target')} ({ev.get('reason')})"
    elif t == "level_up":
        text = f"The player reached level {ev.get('level')}"
    elif t == "heard":
        text = f"{_name(ev.get('speaker'))}: “{ev.get('text', '')}”"
    else:
        rest = {k: v for k, v in ev.items() if k not in ("seq", "type", "t", "game_hours")}
        text = f"{t}: {json.dumps(rest, ensure_ascii=False, separators=(',', ':'))}"
    return f"[Skyrim] {text}"


def tags_for(ev: dict, why: str | None) -> list[str]:
    t = ev.get("type", "")
    tag, prio = KIND_TAGS.get(t, (f"animunculi:{t.replace('_', '-')}", "low"))
    if why is None:
        prio = "low"
    tags = {tag, f"animunculi:priority={prio}"}
    if t == "player_chat":
        tags |= {"chat:dm", "chat:addressed", "chat:private", "chat:from-human"}
    elif t in ("player_dialogue", "dialogue_started") and why:
        tags |= {"chat:addressed", "chat:from-human"}
    return sorted(tags)


# ============================================================================ JSON-RPC over stdio
class StdioRpc:
    """Newline-delimited JSON-RPC 2.0 on stdin/stdout. A reader thread feeds the event loop
    (async stdin pipes are unreliable on Windows); writes are serialized."""

    def __init__(self, loop: asyncio.AbstractEventLoop, stdin=None, stdout=None):
        self.loop = loop
        self.stdin = stdin or sys.stdin.buffer
        self.stdout = stdout or sys.stdout.buffer
        self.inbox: asyncio.Queue = asyncio.Queue()
        self.pending: dict[int, asyncio.Future] = {}
        self.next_id = 0
        self.lock = threading.Lock()
        self.closed = False
        threading.Thread(target=self._reader, name="mcpl-stdin", daemon=True).start()

    def _reader(self) -> None:
        for raw in self.stdin:
            line = raw.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                log.warning("unparseable line from host: %r", line[:200])
                continue
            self.loop.call_soon_threadsafe(self.inbox.put_nowait, msg)
        self.loop.call_soon_threadsafe(self.inbox.put_nowait, None)

    def send(self, msg: dict) -> None:
        data = json.dumps({"jsonrpc": "2.0", **msg}, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
        with self.lock:
            self.stdout.write(data)
            self.stdout.flush()

    def respond(self, req_id: Any, result: Any) -> None:
        self.send({"id": req_id, "result": result})

    def error(self, req_id: Any, code: int, message: str, data: Any = None) -> None:
        err: dict[str, Any] = {"code": code, "message": message}
        if data is not None:
            err["data"] = data
        self.send({"id": req_id, "error": err})

    def notify(self, method: str, params: dict) -> None:
        self.send({"method": method, "params": params})

    async def request(self, method: str, params: dict, timeout: float = 15.0) -> Any:
        self.next_id += 1
        rid = self.next_id
        fut = self.loop.create_future()
        self.pending[rid] = fut
        self.send({"id": rid, "method": method, "params": params})
        try:
            return await asyncio.wait_for(fut, timeout)
        finally:
            self.pending.pop(rid, None)

    def resolve(self, msg: dict) -> None:
        fut = self.pending.get(msg.get("id"))  # type: ignore[arg-type]
        if fut is None or fut.done():
            return
        if "error" in msg:
            err = msg["error"] or {}
            fut.set_exception(RuntimeError(f"{err.get('code')}: {err.get('message')}"))
        else:
            fut.set_result(msg.get("result"))


# ============================================================================ the server
class McplServer:
    def __init__(self, cfg: Config, rpc: StdioRpc):
        self.cfg = cfg
        self.rpc = rpc
        self.mcp, self.session = build_server(cfg)  # the same tools as the plain MCP server
        self.grant = policy.Grant()
        self.policy_answered = False
        self.channel_id = f"skyrim:{cfg.agent}"
        self.channel_registered = False
        self.channel_open = False
        self.player_name = "the player"
        self.thinking: dict[str, float] = {}
        self.push_level = (cfg.extra.get("push") or os.environ.get("ANIMUNCULI_PUSH") or "important").lower()
        self.tasks: set[asyncio.Task] = set()

    # ------------------------------------------------------------------ policy
    def active(self, name: str) -> bool:
        return self.policy_answered and policy.derive(name, FEATURE_SETS[name], self.grant).active

    def chat_live(self) -> bool:
        return self.active("skyrim.chat") and self.channel_registered

    def spawn(self, coro) -> None:
        task = asyncio.create_task(coro)
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)

    async def on_policy(self, params: Any, req_id: Any) -> None:
        form = "notification" if req_id is None else "request"
        try:
            grant, notes = policy.grant_from_update(self.grant, params, form)
        except policy.PolicyError as exc:
            if req_id is not None:
                self.rpc.error(req_id, ERR_INVALID_PARAMS, f"malformed featureSets/update: {exc}")
            # fail closed: nothing granted until a well-formed Request arrives
            self.grant, self.policy_answered = policy.Grant(), False
            self.channel_registered = self.channel_open = False
            return
        for n in notes:
            log.warning("policy: %s", n)
        self.grant = grant
        if req_id is not None:
            self.rpc.respond(req_id, policy.receipt(FEATURE_SETS, grant, CONSEQUENCES))
            self.policy_answered = True  # ready only once the Request is answered (§6.7)
        log.info("policy: grant=%s enabled=%s disabled=%s -> active=%s", grant.effective, grant.enabled,
                 grant.disabled, [n for n in FEATURE_SETS if self.active(n)])
        if not self.active("skyrim.chat"):
            self.channel_registered = False
        if not self.active("skyrim.chat.overhear"):
            self.channel_open = False
        if self.active("skyrim.chat") and not self.channel_registered:
            self.spawn(self.register_channel())

    def descriptor(self) -> dict:
        return {"id": self.channel_id, "type": "skyrim", "label": f"Skyrim: in-game conversation ({self.cfg.agent})",
                "direction": "bidirectional", "address": {"agent": self.cfg.agent}, "initiallyOpen": False,
                "metadata": {"speaks": "published text is spoken aloud in-game by the agent's body"}}

    async def register_channel(self) -> None:
        if not self.grant.has("channels.register"):
            return
        try:
            res = await self.rpc.request("channels/register", {"channels": [self.descriptor()]})
        except Exception as exc:  # noqa: BLE001 - host refused or timed out
            log.warning("channels/register failed: %s", exc)
            return
        results = (res or {}).get("results") or []
        self.channel_registered = any(r.get("id") == self.channel_id and r.get("accepted") for r in results)
        log.info("channel %s registered=%s", self.channel_id, self.channel_registered)

    # ------------------------------------------------------------------ host -> server
    async def handle(self, msg: dict) -> None:
        method = msg.get("method")
        req_id = msg.get("id")
        params = msg.get("params") or {}
        is_request = "id" in msg
        try:
            if method == "initialize":
                wanted = params.get("protocolVersion")
                self.rpc.respond(req_id, {
                    "protocolVersion": wanted if wanted in SUPPORTED_PROTOCOLS else SUPPORTED_PROTOCOLS[-2],
                    "capabilities": {"tools": {"listChanged": False}, "prompts": {"listChanged": False},
                                     "experimental": {"mcpl": MCPL_CAPS}},
                    "serverInfo": {"name": "animunculi", "title": "Animunculi (Skyrim embodiment)", "version": __version__},
                    "instructions": INSTRUCTIONS,
                })
            elif method in ("notifications/initialized", "notifications/cancelled", "notifications/roots/list_changed"):
                pass
            elif method == "ping":
                self.rpc.respond(req_id, {})
            elif method == "tools/list":
                tools = [t.model_dump(by_alias=True, exclude_none=True, mode="json") for t in await self.mcp.list_tools()]
                self.rpc.respond(req_id, {"tools": tools})
            elif method == "tools/call":
                try:
                    res = await self.mcp.call_tool(params.get("name", ""), params.get("arguments") or {})
                    self.rpc.respond(req_id, res.model_dump(by_alias=True, exclude_none=True, mode="json"))
                except ToolError as exc:
                    self.rpc.respond(req_id, {"content": [{"type": "text", "text": str(exc)}], "isError": True})
            elif method == "prompts/list":
                prompts = [p.model_dump(by_alias=True, exclude_none=True, mode="json") for p in await self.mcp.list_prompts()]
                self.rpc.respond(req_id, {"prompts": prompts})
            elif method == "prompts/get":
                res = await self.mcp.get_prompt(params.get("name", ""), params.get("arguments") or {})
                self.rpc.respond(req_id, res.model_dump(by_alias=True, exclude_none=True, mode="json"))
            elif method in ("resources/list", "resources/templates/list"):
                self.rpc.respond(req_id, {"resources": []} if method == "resources/list" else {"resourceTemplates": []})
            elif method == "featureSets/update":
                await self.on_policy(params, req_id if is_request else None)
            elif method == "context/beforeInference":
                self.rpc.respond(req_id, await self.before_inference(params))
            elif method == "inference/lifecycle":
                await self.lifecycle(params)
            elif method == "channels/publish":
                await self.publish(params, req_id if is_request else None)
            elif method == "channels/open":
                await self.open_channel(params, req_id)
            elif method == "channels/close":
                self.require("channels.lifecycle")
                if params.get("channelId") == self.channel_id:
                    self.channel_open = False
                self.rpc.respond(req_id, {"closed": True})
            elif method == "channels/list":
                self.require("channels.register")
                self.rpc.respond(req_id, {"channels": [self.descriptor()] if self.channel_registered else []})
            elif is_request:
                self.rpc.error(req_id, ERR_METHOD_NOT_FOUND, f"method not found: {method}")
        except _Denied as exc:
            if is_request:
                self.rpc.error(req_id, ERR_CAPABILITY_DENIED, "Capability denied", {"capability": exc.path})
        except Exception as exc:  # noqa: BLE001
            log.exception("error handling %s", method)
            if is_request:
                self.rpc.error(req_id, ERR_INTERNAL, str(exc))

    def require(self, path: str) -> None:
        if not (self.policy_answered and self.grant.has(path)):
            raise _Denied(path)

    async def publish(self, params: dict, req_id: Any) -> None:
        self.require("channels.publish")
        if params.get("channelId") != self.channel_id or not self.channel_registered:
            if req_id is not None:
                self.rpc.error(req_id, ERR_UNKNOWN_CHANNEL, f"unknown channel {params.get('channelId')}")
            return
        content = params.get("content")
        if isinstance(content, str):
            text = content
        else:
            text = "\n".join(b.get("text", "") for b in content or [] if isinstance(b, dict) and b.get("type") == "text")
        text = text.strip()
        if not text:
            if req_id is not None:
                self.rpc.respond(req_id, {"delivered": False, "reason": "nothing to say"})
            return
        try:
            res = await self.mcp.call_tool("say", {"text": text})
            said = json.loads("".join(c.text for c in res.content if getattr(c, "type", "") == "text") or "{}")
        except ToolError as exc:
            if req_id is not None:
                self.rpc.error(req_id, ERR_INTERNAL, f"could not speak in game: {exc}")
            return
        if req_id is not None:
            self.rpc.respond(req_id, {"delivered": True, "messageId": f"speech-{said.get('speech_id', '?')}"})

    async def open_channel(self, params: dict, req_id: Any) -> None:
        self.require("channels.lifecycle")
        cid = params.get("channelId") or self.channel_id
        if cid != self.channel_id or not self.channel_registered:
            self.rpc.error(req_id, ERR_UNKNOWN_CHANNEL, f"unknown channel {cid}")
            return
        self.channel_open = True
        limit = int(((params.get("history") or {}).get("limit")) or 0)
        history: list[dict] = []
        if limit > 0:
            try:
                head = int((await self.session.call("events", {"since": 0, "limit": 1})).get("next_seq", 0))
                res = await self.session.call("events", {"since": max(0, head - 300), "types": ["heard"], "limit": 300})
                history = [self.ambient_message(ev) for ev in res.get("events", [])][-limit:]
            except ToolError:
                history = []
        self.rpc.respond(req_id, {"channel": self.descriptor(), "history": history, "historyTruncated": False})

    async def before_inference(self, params: dict) -> dict:
        # write-without-read: this server is never granted observe, and never needs userMessage
        if not self.active("skyrim.presence") or not self.grant.has("contextHooks.beforeInference.inject.beforeUser"):
            return {"contextInjections": []}
        try:
            line = await asyncio.wait_for(self.presence_line(), 3.5)
        except (asyncio.TimeoutError, ToolError, BridgeError):
            line = "[Skyrim] (status unavailable: the game is not answering)"
        return {"featureSet": "skyrim.presence",
                "contextInjections": [{"namespace": "skyrim", "position": "beforeUser", "content": line}]}

    async def presence_line(self) -> str:
        try:
            snap = await self.session.bridge.call("observe", {"agent": self.cfg.agent, "radius_m": 30,
                                                              "max_actors": 5, "max_objects": 0}, timeout=3)
        except BridgeError as exc:
            if exc.code == "disconnected":
                return "[Skyrim] The game is not running (or Animunculi is not loaded)."
            if exc.code == "not_in_game":
                return "[Skyrim] At the main menu or a loading screen."
            return f"[Skyrim] ({exc.code}: {exc.message})"
        parts = []
        when = (snap.get("time") or {}).get("text")
        loc = snap.get("location") or {}
        place = ", ".join(x for x in [loc.get("cell") or loc.get("location"), *(loc.get("within") or [])[:2]] if x)
        head = "[Skyrim" + (f" · {when}" if when else "") + "]"
        me = snap.get("self")
        if me:
            state = [f"{round(float(me.get('health', 1)) * 100)}% health"]
            for flag, word in (("in_combat", "IN COMBAT"), ("waiting", "waiting"), ("sneaking", "sneaking"),
                               ("sitting", "sitting"), ("weapon_drawn", "weapon drawn")):
                if me.get(flag):
                    state.append(word)
            parts.append(f"You ({me.get('name')}): {', '.join(state)}")
        else:
            parts.append(snap.get("note") or "no body bound")
        near = (loc.get("near") or {}).get("text")
        if place or near:
            weather = f", {loc['weather']}" if loc.get("weather") else ""
            where = ", ".join(x for x in (place, near) if x)
            parts.append(f"at {where}{' (indoors)' if loc.get('interior') else ''}{weather}")
        pl = snap.get("player") or {}
        if pl and me:
            self.player_name = pl.get("name") or self.player_name
            ptxt = f"{pl.get('name', 'player')} {pl.get('dist_m', '?')} m {pl.get('bearing_word', '')}".strip()
            if pl.get("health") is not None and float(pl["health"]) < 0.99:
                ptxt += f" ({round(float(pl['health']) * 100)}%)"
            if pl.get("in_combat"):
                ptxt += " fighting"
            parts.append(ptxt)
        near = [a for a in snap.get("actors") or [] if not a.get("dead")][:4]
        if near:
            parts.append("near: " + ", ".join(
                f"{a.get('name')} {a.get('dist_m')} m{' HOSTILE' if a.get('hostile_to_player') or a.get('hostile_to_you') else ''}"
                for a in near))
        return f"{head} " + " · ".join(parts)

    async def lifecycle(self, params: dict) -> None:
        if not self.active("skyrim.thinking"):
            return
        inf = str(params.get("inferenceId", ""))
        phase = params.get("phase")
        now = time.monotonic()
        # safety timeout: a terminal phase is best-effort (§10.5)
        self.thinking = {k: v for k, v in self.thinking.items() if now - v < 180}
        if phase == "started":
            self.thinking[inf] = now
        elif phase in ("completed", "aborted", "failed"):
            self.thinking.pop(inf, None)
        try:
            await self.session.bridge.call("presence", {"agent": self.cfg.agent, "thinking": bool(self.thinking)}, timeout=3)
        except BridgeError:
            pass  # older plugin or game not running: purely cosmetic

    # ------------------------------------------------------------------ world -> host
    def ambient_message(self, ev: dict) -> dict:
        speaker = ev.get("speaker") or {}
        return {"channelId": self.channel_id, "messageId": f"anm-heard-{ev.get('seq')}",
                "author": {"id": speaker.get("id", "npc"), "name": speaker.get("name", "someone")},
                "timestamp": _iso(ev.get("t")),
                "content": [{"type": "text", "text": ev.get("text", "")}],
                "tags": ["chat:ambient"] + (["animunculi:to-player"] if ev.get("to_player") else []),
                "metadata": {"dist_m": ev.get("dist_m")}}

    async def incoming(self, messages: list[dict]) -> None:
        if not (self.chat_live() and self.grant.has("channels.incoming")):
            return
        try:
            await self.rpc.request("channels/incoming", {"messages": messages})
        except Exception as exc:  # noqa: BLE001
            log.warning("channels/incoming failed: %s", exc)

    async def push(self, ev: dict, why: str | None, epoch: str) -> None:
        if not (self.active("skyrim.events") and self.grant.has("pushEvents")):
            return
        origin: dict[str, Any] = {"source": "animunculi", "agent": self.cfg.agent, "type": ev.get("type"),
                                  "seq": ev.get("seq")}
        if ev.get("type") == "player_chat":
            origin.update({"authorName": self.player_name, "authorId": "player"})
        params = {"featureSet": "skyrim.events", "eventId": f"anm-{epoch}-{ev.get('seq')}", "timestamp": _iso(ev.get("t")),
                  "origin": origin,
                  "payload": {"content": [{"type": "text", "text": render(ev, why)}],
                              "event": {k: v for k, v in ev.items() if k != "t"}},
                  "tags": tags_for(ev, why)}
        try:
            res = await self.rpc.request("push/event", params)
            if isinstance(res, dict) and res.get("accepted") is False:
                log.info("push/event not accepted: %s", res.get("reason"))
        except Exception as exc:  # noqa: BLE001
            log.warning("push/event failed: %s", exc)

    async def route(self, ev: dict, epoch: str) -> None:
        agent = self.cfg.agent
        t = ev.get("type")
        why = importance(ev, agent)
        if t == "player_chat" and (not ev.get("agent") or ev.get("agent") == agent):
            if self.chat_live():
                await self.incoming([{"channelId": self.channel_id, "messageId": f"anm-{epoch}-{ev.get('seq')}",
                                      "author": {"id": "player", "name": self.player_name},
                                      "timestamp": _iso(ev.get("t")),
                                      "content": [{"type": "text", "text": ev.get("text", "")}],
                                      "tags": ["chat:dm", "chat:addressed", "chat:private", "chat:from-human"],
                                      "metadata": {"source": ev.get("source")}}])
                return
        elif t == "heard":
            if self.chat_live() and self.channel_open and self.active("skyrim.chat.overhear"):
                await self.incoming([self.ambient_message(ev)])
            if self.push_level != "all":
                return
        elif t == "player_dialogue" and not why:
            if self.chat_live() and self.channel_open and self.active("skyrim.chat.overhear"):
                await self.incoming([{"channelId": self.channel_id, "messageId": f"anm-{epoch}-{ev.get('seq')}",
                                      "author": {"id": "player", "name": self.player_name},
                                      "timestamp": _iso(ev.get("t")),
                                      "content": [{"type": "text", "text": f"(to {_name(ev.get('to'))}) {ev.get('text', '')}"}],
                                      "tags": ["chat:ambient", "chat:from-human"]}])
            if self.push_level != "all":
                return
        if why or self.push_level == "all":
            await self.push(ev, why, epoch)

    async def pump(self) -> None:
        """Follow the plugin's event log on a connection of its own and deliver what the grant allows."""
        # Always following (from "now" at connect), and gating per event in route(): delivery starts
        # the moment the grant allows it, without a connect race and without replaying a backlog.
        bridge = Bridge(self.cfg)
        cursor: int | None = None
        epoch = "0"
        while True:
            try:
                if cursor is None:
                    await bridge.ensure_connected()
                    cursor = int(bridge.hello.get("event_seq", 0))
                    epoch = str(int(time.time()))
                    try:
                        st = await bridge.call("status", {}, timeout=3)
                        self.player_name = (st.get("player") or {}).get("name") or self.player_name
                    except BridgeError:
                        pass
                res = await bridge.call("wait_events", {"since": cursor, "limit": 100, "timeout_ms": 15000}, timeout=25)
            except BridgeError as exc:
                if exc.code in ("disconnected", "timeout"):
                    await bridge.close()
                    cursor = None
                    await asyncio.sleep(3.0)
                    continue
                log.warning("event pump: %s", exc)
                await asyncio.sleep(1.0)
                continue
            cursor = res.get("next_seq", cursor)
            for ev in res.get("events", []):
                if ev.get("type") == "game_loaded":
                    self.spawn(self._refresh_player(bridge))
                await self.route(ev, epoch)

    async def _refresh_player(self, bridge: Bridge) -> None:
        try:
            st = await bridge.call("status", {}, timeout=3)
            self.player_name = (st.get("player") or {}).get("name") or self.player_name
        except BridgeError:
            pass

    async def run(self) -> None:
        self.spawn(self.pump())
        while True:
            msg = await self.rpc.inbox.get()
            if msg is None:
                break  # host closed stdin
            if "method" not in msg and "id" in msg:
                self.rpc.resolve(msg)
                continue
            self.spawn(self.handle(msg))


class _Denied(Exception):
    def __init__(self, path: str):
        super().__init__(path)
        self.path = path


def main() -> None:
    logging.basicConfig(level=logging.INFO, stream=sys.stderr, format="%(asctime)s %(name)s %(levelname)s %(message)s")

    async def amain() -> None:
        rpc = StdioRpc(asyncio.get_running_loop())
        await McplServer(Config.load(), rpc).run()

    try:
        asyncio.run(amain())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
