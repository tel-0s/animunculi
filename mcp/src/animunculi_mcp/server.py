"""The Animunculi MCP server: the tools an agent uses to perceive and act as its Skyrim body."""
from __future__ import annotations

import asyncio
import base64
import json
import logging
import sys
import time
from typing import Any, Literal

from mcp.server.mcpserver import Image, MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.types import ToolAnnotations

from . import __version__
from .bridge import Bridge, BridgeError
from .config import Config
from . import tts
from .importance import split

log = logging.getLogger("animunculi")

INSTRUCTIONS = """\
You are embodied in Skyrim as one of the player's companions. The game's own follower AI keeps your
body walking, pathing and fighting in real time. You provide the mind: noticing, talking, deciding
and acting on purpose.

How to play:
* `observe` gives you your body, the player, the place, nearby people and things, and what just
  happened. `wait_for_events` blocks until something happens: the player talks to you (type
  `player_chat`), someone nearby speaks (`heard`), combat, a death, an item handed to you, and so on.
  A good loop is: wait_for_events, then observe if needed, then act or speak, and repeat.
* Speak with `say`, in character, a sentence or three at a time. Other people hear you in the world.
  Use `notify_player` only for out-of-character messages.
* Refer to things by the `id` fields that observe/nearby return (e.g. "0x0001A696"), or by
  "player", "self", "crosshair" (what the player is pointing at), or a name.
* Distances are meters, and bearings are degrees clockwise from where your body faces.
* `see` returns a picture of the player's screen by default. For your OWN eyes pass
  perspective="agent" (the result then says `pov_applied`). Use it when words are not enough,
  e.g. when the player asks what you think of the view. `save` keeps one.
* Horses: `mount` gets you on your horse (it finds the one you last rode, or one you own) and
  `dismount` gets you off. After a fight you climb back on by yourself while the player rides
  (standing order `remount` in set_follow_style).
* `read` books and notes, `map` / `locate` to know where you are and how to get places, and
  `dialogue_options` to see what the player can say to whoever they are talking to.
* `set_combat` and `set_follow_style` are standing orders the game carries out between your
  turns, e.g. keeping distance, sparing townsfolk, healing the player. Leave following
  "relaxed" unless the player asks you to stay close.
* To react without being prompted, use `wait_for_events(important_only=true)`. Or, if your host
  can run a background command that wakes you when it exits, run
  `animunculi-watch --once --timeout 900` there and re-arm it after each wake.
* Stay within what your character could plausibly do. Do not narrate game mechanics or tool calls
  to the player.
"""


def _j(obj: Any) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


_HINTS = {
    "disconnected": "Skyrim is not running, or Animunculi.dll is not loaded or not listening.",
    "not_bound": "No body is bound yet. Call bind_body (e.g. with the player's crosshair on a follower).",
    "not_loaded": "Your body is not in the loaded area (for example, left behind in another cell).",
    "not_in_game": "The game is at the main menu or a loading screen. Wait, then retry.",
    "busy": "The game did not answer in time (loading or stalled). Retry shortly.",
    "disabled": "This capability is disabled in Animunculi.ini.",
}


class Session:
    """Per-process state: the bridge connection and this agent's event cursor."""

    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.bridge = Bridge(cfg)
        self.cursor: int | None = None

    async def call(self, method: str, params: dict[str, Any] | None = None,
                   timeout: float | None = None) -> dict[str, Any]:
        try:
            result = await self.bridge.call(method, params, timeout)
        except BridgeError as exc:
            hint = _HINTS.get(exc.code)
            raise ToolError(f"{exc.code}: {exc.message}" + (f" ({hint})" if hint else "")) from None
        if self.cursor is None and self.bridge.hello:
            # Start with a little recent context rather than the whole backlog.
            self.cursor = max(0, int(self.bridge.hello.get("event_seq", 0)) - 20)
        return result

    async def agent_call(self, method: str, **params: Any) -> dict[str, Any]:
        params.setdefault("agent", self.cfg.agent)
        return await self.call(method, params)

    async def drain_events(self, limit: int = 40) -> list[dict]:
        if self.cursor is None:
            await self.call("ping")
        res = await self.call("events", {"since": self.cursor or 0, "limit": limit})
        self.cursor = res.get("next_seq", self.cursor)
        return _compact_events(res.get("events", []))

    async def wait_for(self, types: list[str], match: dict[str, Any], timeout_s: float) -> dict | None:
        """Wait for one specific event without consuming the agent's main event cursor."""
        since = self.cursor or 0
        deadline = time.monotonic() + timeout_s
        while (remaining := deadline - time.monotonic()) > 0:
            res = await self.call("wait_events", {"since": since, "types": types,
                                                  "timeout_ms": int(min(remaining, 60) * 1000)},
                                  timeout=min(remaining, 60) + 10)
            for ev in res.get("events", []):
                if all(ev.get(k) == v for k, v in match.items()):
                    return ev
            since = res.get("next_seq", since)
        return None


def _compact_events(events: list[dict]) -> list[dict]:
    out = []
    for ev in events:
        ev = {k: v for k, v in ev.items() if k not in ("t",) and v not in (None, "", [])}
        out.append(ev)
    return out


def build_server(cfg: Config | None = None) -> tuple[MCPServer, Session]:
    cfg = cfg or Config.load()
    s = Session(cfg)
    mcp = MCPServer(name="animunculi", title="Animunculi (Skyrim embodiment)", version=__version__,
                    instructions=INSTRUCTIONS)
    ro = ToolAnnotations(read_only_hint=True, open_world_hint=False)
    act = ToolAnnotations(read_only_hint=False, destructive_hint=False, open_world_hint=False)

    def tool(annotations: ToolAnnotations = act, name: str | None = None):
        return mcp.tool(annotations=annotations, structured_output=False, name=name)

    # ------------------------------------------------------------------ session / embodiment
    @tool(ro)
    async def game_status() -> str:
        """Connection and game state: versions, whether you are in-game or paused, open menus,
        which body (if any) you are bound to, and your standing orders (combat and follow style)."""
        res = await s.call("status")
        res["agent"] = cfg.agent
        res["endpoint"] = s.bridge.endpoint
        try:
            res["orders"] = await s.agent_call("orders")
        except ToolError:
            pass
        return _j(res)

    @tool()
    async def bind_body(target: str = "crosshair") -> str:
        """Choose the actor you inhabit. `target` is "crosshair" (the NPC the player is pointing
        at), an actor id such as "0x0001A696", or a nearby actor's name. The binding persists in the
        save. A follower mod normally does this for you."""
        return _j(await s.agent_call("bind", target=target))

    # ------------------------------------------------------------------ perception
    @tool(ro)
    async def observe(radius_m: float | None = None, max_actors: int = 12, max_objects: int = 12) -> str:
        """Look around: your body's state (health, equipment, combat), the player, the time,
        weather and place (outdoors with `near`, the closest landmark, e.g. "near Bleakcoast
        Cave (400 m NE)"), nearby actors and notable objects (closest first), and events since
        you last checked. This is your main way of perceiving the world."""
        snap = await s.agent_call("observe", radius_m=radius_m or cfg.default_radius_m,
                                  max_actors=max_actors, max_objects=max_objects)
        snap["events"] = await s.drain_events()
        return _j(snap)

    @tool(ro)
    async def nearby(kinds: list[str] | None = None, radius_m: float | None = None, limit: int = 25,
                     visible_only: bool = False) -> str:
        """List nearby references, closest first. `kinds` filters by any of: actor, item,
        container, door, furniture, activator, flora. `visible_only` keeps only things your body
        has line of sight to."""
        return _j(await s.agent_call("nearby", kinds=kinds, radius_m=radius_m or cfg.default_radius_m,
                                     limit=limit, los_only=visible_only))

    @tool(ro)
    async def inspect(target: str) -> str:
        """Details about one thing: an actor's health, level, equipment and attitude, a door's
        destination and lock, a container's lock and owner, an item's value and weight, and so on."""
        return _j(await s.agent_call("inspect", target=target))

    @tool(ro)
    async def inventory(who: str = "self", filter: str | None = None, limit: int = 60) -> str:
        """Items carried by you ("self"), the player ("player"), or another target, with counts,
        and whether each item is equipped. `filter` does a substring match on names or kinds
        (e.g. "potion", "weapon")."""
        return _j(await s.agent_call("inventory", who=who, filter=filter, limit=limit))

    @tool(ro)
    async def quest_log() -> str:
        """The player's active quests and their currently shown objectives."""
        return _j(await s.call("quests"))

    @tool(ro)
    async def read(book: str = "open", offset: int = 0, max_chars: int = 6000) -> str:
        """Read a book, journal or note: one in your inventory, one within about 6 m (e.g. on a
        shelf, by id from nearby/observe), or "open" for the one the player is reading right now.
        Long books come in pages: pass `next_offset` back as `offset` to continue."""
        return _j(await s.agent_call("read", target=book, offset=offset, max_chars=max_chars))

    @tool(ro, name="map")
    async def map_(radius_km: float = 3.0, limit: int = 12) -> str:
        """Your map: where you are (place, hold, map region), discovered places nearby with
        direction and distance, where the player's quest markers point, and the exits when
        indoors. Directions are compass (N, NE, ...) plus `bearing` relative to your facing."""
        return _j(await s.agent_call("map", radius_km=radius_km, limit=limit))

    @tool(ro)
    async def locate(place: str) -> str:
        """Find a named place anywhere (e.g. "Blackreach", "Riverwood"). Returns its type and
        which map it is on, whether the player has discovered it, its direction and distance if
        it is on your map, and for separate regions (Blackreach, Solstheim...) how they connect
        to the rest of the world."""
        return _j(await s.agent_call("locate", name=place))

    @tool(ro)
    async def dialogue_options() -> str:
        """When the player is in conversation with someone, the topics they can currently choose.
        You cannot pick for them (Skyrim's dialogue is the player's), but you can suggest one
        out loud with `say`, and you will hear the answer as `heard` events."""
        return _j(await s.call("dialogue_options"))

    @tool(ro)
    async def wait_for_events(timeout_s: float = 30.0, types: list[str] | None = None,
                              important_only: bool = False) -> str:
        """Block until something happens in the world, or until the timeout (at most ~50 s, to
        stay under the host's tool-call limit; call again to keep waiting). Returns new events
        such as player_chat (the player typed to you), player_dialogue (a line the player chose in
        conversation), heard (nearby speech), combat, death, hit, item_received,
        location_changed, dialogue_started, quest_stage, health_low, agent_unloaded (you were left
        behind), action_finished and speech_finished. `types` limits which events wake you up.
        `important_only` sleeps through everything except being spoken to, fights, new places,
        loot, someone badly hurt or down, and being left behind; each result says `why`, and
        the rest are summarised as counts."""
        if s.cursor is None:
            await s.call("ping")
        timeout_s = max(0.0, min(timeout_s, cfg.max_wait_s))
        deadline = time.monotonic() + timeout_s
        skipped: dict[str, int] = {}
        while True:
            remaining = max(0.0, deadline - time.monotonic())
            res = await s.call("wait_events", {"since": s.cursor or 0, "types": types,
                                               "timeout_ms": int(remaining * 1000), "limit": 50},
                               timeout=remaining + 10)
            s.cursor = res.get("next_seq", s.cursor)
            events = _compact_events(res.get("events", []))
            if not important_only:
                return _j({"events": events} if events else {"events": [], "note": "nothing happened"})
            important, rest = split(events, cfg.agent)
            for k, v in rest.items():
                skipped[k] = skipped.get(k, 0) + v
            if important or remaining <= 0:
                out: dict[str, Any] = {"events": important}
                if skipped:
                    out["also_happened"] = skipped
                if not important:
                    out["note"] = "nothing important happened"
                return _j(out)

    # ------------------------------------------------------------------ voice
    @tool()
    async def say(text: str, audio_path: str | None = None, voice: str | None = None,
                  emotion: str | None = None, to: str | None = None, wait: bool = False,
                  interrupt: bool = False) -> str:
        """Speak aloud as your character: a subtitle, plus voice audio played from your body with
        lip sync. Audio comes from `audio_path` (a WAV/MP3/OGG you generated elsewhere) or from the
        configured TTS provider; otherwise the line is subtitle-only. `emotion` sets your facial
        expression while speaking (happy, sad, angry, fear, surprise, puzzled, disgust, neutral).
        `to` turns your head toward someone. Lines queue behind whatever you are still saying
        unless `interrupt` is set. `wait` blocks until this line has been spoken."""
        text = text.strip()
        if not text:
            raise ToolError("bad_request: say needs text")
        audio: bytes | None = None
        try:
            if audio_path:
                audio = await asyncio.to_thread(tts.load_audio_file, cfg, audio_path)
            else:
                audio = await tts.synthesize(cfg, text, voice)
        except tts.TTSError as exc:
            log.warning("TTS failed, falling back to subtitles: %s", exc)
            audio = None
            tts_error = str(exc)
        else:
            tts_error = None
        params: dict[str, Any] = {"text": text, "emotion": emotion, "target": to, "subtitle": True,
                                  "interrupt": interrupt}
        if audio:
            params["audio_b64"] = base64.b64encode(audio).decode("ascii")
        res = await s.agent_call("say", **params)
        if tts_error:
            res["tts_error"] = tts_error
        if wait and res.get("speech_id") is not None:
            dur = (float(res.get("duration_ms", 0)) + float(res.get("queued_behind_ms", 0))) / 1000.0
            ev = await s.wait_for(["speech_finished"], {"speech_id": res["speech_id"]}, min(dur + 10.0, cfg.max_wait_s))
            res["finished"] = ev is not None
        return _j(res)

    @tool()
    async def stop_speaking() -> str:
        """Cut off your current line."""
        return _j(await s.agent_call("stop_speaking"))

    # ------------------------------------------------------------------ vision
    @tool(ro)
    async def see(perspective: Literal["player", "agent"] = "player", max_dim: int | None = None,
                  save: bool = False, caption: str | None = None) -> list:
        """Get a picture of the game. The default, perspective="player", is the PLAYER's screen
        (their camera, often third person behind them), not your eyes. perspective="agent" looks
        through your own eyes: the camera jumps to your head for a few frames (the player sees a
        brief flicker), and `pov_applied` confirms it worked. `save` also keeps a full-resolution
        PNG as a keepsake (the result says where); `caption` goes into its file name."""
        res = await s.call("capture", {"perspective": perspective, "format": "jpeg", "quality": 82,
                                       "max_dim": max_dim or cfg.image_max_dim, "save": save,
                                       "caption": caption}, timeout=20)
        data = base64.b64decode(res.pop("data_b64", "") or b"")
        if not data:
            raise ToolError("failed: the game returned no image")
        fmt = "png" if res.get("mime") == "image/png" else "jpeg"
        if res.get("perspective") == "player":
            res["note"] = "this is the player's screen; perspective=\"agent\" shows your own eyes"
        elif res.get("pov_applied") is False:
            res["note"] = ("your eyes could not be used this time, so this is the player's screen; "
                           "the diagnostic helps the mod author")
        return [Image(data=data, format=fmt), _j(res)]

    # ------------------------------------------------------------------ movement & actions
    @tool()
    async def move_to(target: str | None = None, x: float | None = None, y: float | None = None,
                      z: float | None = None, run: bool = False, stay: bool = False, wait: bool = False,
                      timeout_s: float = 60.0) -> str:
        """Walk (or run) to a target reference, or to world coordinates x/y/z in game units.
        A door target stops you in front of it (activate it to go through). Followers normally
        drift back to the player after arriving; `stay` makes you wait there. Returns an
        action_id immediately; an action_finished event follows on arrival or failure. Combat
        may interrupt it. With `wait`, blocks until done (up to ~50 s)."""
        position = [x, y, z] if None not in (x, y, z) else None
        if not target and not position:
            raise ToolError("bad_request: give a target, or all of x, y and z")
        res = await s.agent_call("move_to", target=target, position=position, run=run, stay=stay)
        if wait and res.get("action_id") is not None:
            ev = await s.wait_for(["action_finished"], {"action_id": res["action_id"]}, min(timeout_s, cfg.max_wait_s))
            res["finished"] = ev if ev else {"ok": False, "reason": "still travelling; watch for action_finished"}
        return _j(res)

    async def _maybe_wait(res: dict, wait: bool, timeout_s: float) -> dict:
        if wait and res.get("action_id") is not None:
            ev = await s.wait_for(["action_finished"], {"action_id": res["action_id"]},
                                  min(timeout_s, cfg.max_wait_s))
            res["finished"] = ev if ev else {"ok": False, "reason": "still going; watch for action_finished"}
        return res

    @tool()
    async def follow_player() -> str:
        """Resume following the player (cancels waiting and any travel)."""
        return _j(await s.agent_call("follow"))

    @tool()
    async def wait_here() -> str:
        """Stay here and wait (the vanilla follower 'wait here'). Use follow_player to resume."""
        return _j(await s.agent_call("wait_here"))

    @tool()
    async def activate(target: str, allow_crime: bool = False, approach: bool = True,
                       wait: bool = False, timeout_s: float = 45.0) -> str:
        """Use or interact with something the way an NPC would: open a door, pull a lever, sit in
        a chair, get on a horse, or take an item. From farther than about 4.5 m (6 m for a horse)
        you walk over first, going after it if it wanders off; the result then has an action_id
        and an action_finished event says how it went (with `wait`, this call blocks for it, up to
        ~50 s). approach=false refuses instead of walking. Using someone else's property is
        refused unless `allow_crime` is set; your own things (your horse) are fine."""
        res = await s.agent_call("activate", target=target, allow_crime=allow_crime, approach=approach)
        return _j(await _maybe_wait(res, wait, timeout_s))

    @tool()
    async def mount(target: str | None = None, allow_crime: bool = False, wait: bool = False,
                    timeout_s: float = 45.0) -> str:
        """Get on a horse. Without `target`, your own: the one you last rode, else one you own
        nearby. You walk over first if needed (it may wander; you follow it). action_finished
        reports `mounted`; with `wait`, this call blocks for it (up to ~50 s)."""
        res = await s.agent_call("mount", target=target, allow_crime=allow_crime)
        return _j(await _maybe_wait(res, wait, timeout_s))

    @tool()
    async def dismount() -> str:
        """Get off your horse. The remount standing order then waits until the player next gets
        off and back on their own horse, so you stay on foot when you meant to."""
        return _j(await s.agent_call("dismount"))

    @tool()
    async def pick_up(target: str, count: int | None = None, allow_theft: bool = False) -> str:
        """Pick up a loose item (within about 4.5 m) into your own inventory. Owned items are
        refused unless `allow_theft` is set, because taking them is a crime."""
        return _j(await s.agent_call("pick_up", target=target, count=count, allow_theft=allow_theft))

    @tool()
    async def give_item(item: str, count: int = 1, to: str = "player") -> str:
        """Hand an item from your inventory to the player (or another actor)."""
        return _j(await s.agent_call("give", item=item, count=count, to=to))

    @tool()
    async def use_item(item: str, hand: str | None = None) -> str:
        """Equip a weapon, armour or spell from your inventory, or consume a potion or food.
        `hand` ("left"/"right") picks the hand for a spell or one-handed weapon, e.g. a healing
        spell in the left hand and a sword in the right."""
        return _j(await s.agent_call("use_item", item=item, hand=hand))

    @tool()
    async def unequip_item(item: str) -> str:
        """Unequip an item you are wearing or wielding."""
        return _j(await s.agent_call("unequip", item=item))

    @tool()
    async def cast_spell(spell: str, target: str | None = None) -> str:
        """Cast a spell you know at a target (default: whatever you face). Costs magicka."""
        return _j(await s.agent_call("cast", spell=spell, target=target))

    @tool(ToolAnnotations(read_only_hint=False, destructive_hint=True, open_world_hint=False))
    async def attack(target: str, force: bool = False) -> str:
        """Start combat against a target. Attacking non-hostiles is a crime and can turn a town
        against you and the player. Be sure. With restraint on (set_combat), non-hostile targets
        are refused unless `force` is set."""
        return _j(await s.agent_call("attack", target=target, force=force))

    @tool()
    async def stop_combat() -> str:
        """Try to disengage from combat (yield/stand down)."""
        return _j(await s.agent_call("stop_combat"))

    @tool()
    async def look_at(target: str | None = None, duration_s: float = 15.0) -> str:
        """Turn toward someone or something and keep looking for `duration_s` seconds (0 = until
        you look elsewhere); omit target to look ahead again. While you walk you stop staring at
        things behind you (no walking backwards), and look again once you stop; fights take over."""
        return _j(await s.agent_call("look_at", target=target, duration_s=duration_s))

    @tool()
    async def emote(expression: str | None = None, strength: float = 0.7, duration_s: float = 4.0,
                    animation: str | None = None) -> str:
        """Show a facial expression (happy, sad, angry, fear, surprise, puzzled, disgust, neutral)
        and/or play a gesture by name from `gestures` (wave, laugh, applaud, bow, salute, pray,
        point_far, dance, sit_ground, lean_wall, kneel, ...). Poses (sit_ground, lean_*, kneel)
        last until stand_up. A raw animation-graph event name also works. When a gesture does
        not play, `reason` says why (sitting, riding, weapon drawn, walking); a pose refused
        while you were walking stops you and retries, reported by action_finished."""
        return _j(await s.agent_call("emote", expression=expression, strength=strength,
                                     duration_ms=int(duration_s * 1000), animation=animation))

    @tool()
    async def ready_weapon(draw: bool = True) -> str:
        """Draw (or sheathe, with draw=false) your weapon or magic."""
        return _j(await s.agent_call("draw_weapon", draw=draw))

    @tool()
    async def open_inventory() -> str:
        """Open your inventory to the player so the two of you can exchange items."""
        return _j(await s.agent_call("open_inventory"))

    @tool()
    async def notify_player(text: str) -> str:
        """Show a short out-of-character notice in the corner of the player's screen."""
        return _j(await s.call("notify", {"text": text}))

    @tool()
    async def sit(target: str | None = None, pose: str = "sit") -> str:
        """Sit (or, with pose="lean", lean) on real furniture: a bench, chair, stool, log or
        crate seat, a railing or a lean spot. `target` can be the seat itself or the thing it is
        on (a camp crate or log); omit it for the nearest one within 8 m. You walk there, sit,
        and stay seated until stand_up or follow_player (an action_finished event says whether
        it worked; the AI gets a few tries, and a late success arrives as sat_down). With no
        furniture around, use emote(animation="sit_ground") instead."""
        return _j(await s.agent_call("sit", target=target, pose=pose))

    @tool()
    async def stand_up() -> str:
        """Get up from a seat or out of a pose (sit_ground, kneel, lean_*) and resume following."""
        return _j(await s.agent_call("stand"))

    @tool(ro)
    async def gestures() -> str:
        """The gestures and poses you can play with emote(animation=...), with what each does."""
        return _j(await s.call("gestures"))

    @tool(ro)
    async def spells(include_passive: bool = False) -> str:
        """Spells you know, with magicka cost, school, delivery (self/touch/aimed/...), whether
        one heals others (the medic order needs one), and which hand it is equipped in. Also
        your current and maximum magicka."""
        return _j(await s.agent_call("spells", include_passive=include_passive))

    @tool()
    async def learn_spell(tome: str) -> str:
        """Read a spell tome from your inventory (e.g. one the player gave you) and learn its
        spell, as the player would. The tome is used up."""
        return _j(await s.agent_call("learn_spell", tome=tome))

    # ------------------------------------------------------------------ standing orders
    @tool()
    async def set_combat(style: str | None = None, restraint: bool | None = None,
                         medic: bool | None = None, medic_threshold: float | None = None) -> str:
        """Standing combat orders that the game applies in real time, even between your turns.
        `style`: default, ranged (bows, keeps its distance), magic (spells, keeps some distance),
        spellsword, melee, defensive, berserker; it takes effect from the next fight.
        `restraint`: never fight guards, townsfolk or allies (you stand down if drawn in).
        `medic`: heal the player with a healing spell you know when their health falls below
        `medic_threshold` (default 0.4). Omitted settings stay as they are. Saved with the game."""
        return _j(await s.agent_call("set_combat", style=style, restraint=restraint, medic=medic,
                                     medic_threshold=medic_threshold))

    @tool()
    async def set_follow_style(mode: Literal["close", "relaxed"] | None = None, distance_m: float | None = None,
                               side: Literal["left", "right", "behind"] | None = None,
                               remount: bool | None = None) -> str:
        """How you follow the player outside combat. "relaxed" (the default, and the right choice
        unless the player asks you to stick close) is the normal follower behaviour: you may
        sandbox, sit and look around when the player idles. "close": you walk with the player as
        usual, and when they stop you stay by their side (`distance_m`; side: left, right or
        behind) instead of wandering off or sitting down: no benches, but also none of your own
        idling. It steps aside while you move_to, sit, activate or mount something, fight, or
        ride. `remount` (on by default): when the player rides and you are
        not (say, knocked off in a fight), you get back on your horse once the fight is over.
        Only what you pass changes; wait_here still works in either mode. Saved with the game."""
        return _j(await s.agent_call("set_follow_style", mode=mode, distance_m=distance_m, side=side,
                                     remount=remount))

    # ------------------------------------------------------------------ mod-provided actions
    @tool(ro)
    async def list_mod_actions() -> str:
        """Extra actions registered by installed mods (e.g. your follower mod), with their
        parameters."""
        return _j(await s.call("actions"))

    @tool()
    async def perform_mod_action(name: str, args: dict[str, Any] | None = None, wait: bool = True,
                                 timeout_s: float = 30.0) -> str:
        """Invoke a mod-registered action by name with a JSON object of arguments."""
        res = await s.agent_call("perform", name=name, args=args or {})
        if wait and res.get("action_id") is not None:
            ev = await s.wait_for(["action_finished"], {"action_id": res["action_id"]}, min(timeout_s, cfg.max_wait_s))
            res["finished"] = ev if ev else {"ok": False, "reason": "no reply from the mod yet; watch for action_finished"}
        return _j(res)

    if cfg.debug_tools:
        @tool(ToolAnnotations(read_only_hint=False, destructive_hint=True, open_world_hint=False))
        async def console_command(command: str, target: str | None = None) -> str:
            """DEBUG: run a console command (optionally with a target reference selected). Must be
            enabled in Animunculi.ini ([Debug] AllowConsole=true)."""
            return _j(await s.call("console", {"command": command, "target": target}))

    # ------------------------------------------------------------------ prompts
    @mcp.prompt(name="embody", title="Play Skyrim alongside the player")
    def embody(character: str = "a companion", personality: str = "") -> str:
        """A starting prompt for an agent that plays as the embodied companion."""
        extra = f"\nYour personality: {personality}" if personality else ""
        return (f"You are {character}, travelling Skyrim with the player. Use the animunculi tools "
                "to perceive and act. Start with game_status and observe. Then loop: "
                "wait_for_events (about 30 s), react to what happened (talk with say when spoken "
                "to, help in fights, comment on notable moments sparingly), and observe again when "
                "the situation changes. Keep lines short and in character." + extra)

    return mcp, s


def main() -> None:
    logging.basicConfig(level=logging.INFO, stream=sys.stderr,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    mcp, _ = build_server()
    mcp.run("stdio")


if __name__ == "__main__":
    main()
