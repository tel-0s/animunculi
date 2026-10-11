"""Which world events are worth waking an agent for.

Used by `wait_for_events(important_only=True)` and the `animunculi-watch` background watcher. The
rule of thumb is: things a companion would look up for. Being spoken to, a fight starting,
arriving somewhere new, being handed loot, someone going down, being left behind.
"""
from __future__ import annotations

from collections import Counter
from typing import Any


def _is_agent(who: Any, agent: str) -> bool:
    return isinstance(who, dict) and who.get("agent") == agent


def _is_player(who: Any) -> bool:
    return isinstance(who, dict) and bool(who.get("is_player"))


def importance(ev: dict[str, Any], agent: str) -> str | None:
    """Why `ev` matters to `agent`, or None if it can wait."""
    t = ev.get("type")
    if t == "player_chat":
        target = ev.get("agent")
        return "the player spoke to you" if not target or target == agent else None
    if t == "dialogue_started" and _is_agent(ev.get("with"), agent):
        return "the player started a conversation with you"
    if t == "player_dialogue" and _is_agent(ev.get("to"), agent):
        return "the player said something to you in conversation"
    if t == "combat" and ev.get("state") == "combat":
        return "a fight started"
    if t == "location_changed" and ev.get("to"):
        return f"arrived at {ev['to']}"
    if t == "item_received" and ev.get("agent") == agent and ev.get("how") in (
            "given_by_player", "picked_up", "taken_from_container"):
        return f"you got {ev.get('item', 'an item')}"
    if t == "health_low" and (_is_agent(ev.get("who"), agent) or _is_player(ev.get("who"))):
        return "someone is badly hurt"
    if t == "bleedout" and (_is_agent(ev.get("actor"), agent) or _is_player(ev.get("actor"))):
        return "someone is down"
    if t == "death" and (_is_agent(ev.get("victim"), agent) or _is_player(ev.get("victim"))):
        return "a death in the party"
    if t == "agent_unloaded" and ev.get("agent") == agent:
        return "you were left behind"
    if t == "restrained" and ev.get("agent") == agent:
        return "you held back from attacking someone who is not hostile"
    if t == "level_up":
        return "the player levelled up"
    if t == "custom":
        return "a mod sent you an event"
    return None


def split(events: list[dict], agent: str) -> tuple[list[dict], dict[str, int]]:
    """Important events (annotated with `why`) and a count of the rest by type."""
    important, rest = [], Counter()
    for ev in events:
        why = importance(ev, agent)
        if why:
            important.append({**ev, "why": why})
        else:
            rest[ev.get("type", "?")] += 1
    return important, dict(rest)
