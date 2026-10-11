# Changelog

Plugin versions (`Animunculi.dll`) first; the MCP package (`animunculi-mcp`) moves alongside.
The first line of `Animunculi.log` says which plugin build is loaded.

## 0.3.6 (MCP 0.4.4)

* **Close follow no longer moonwalks.** `KeepOffsetFromActor` fixes the follower's facing when the
  call is issued, so after the player turned, the follower kept that facing and ran backwards;
  re-issuing it did not reset the facing. Close follow now lets the normal follower AI walk the
  body while the player travels, and holds it beside the player only once they have stood still
  for 2 s (wide radius: turning on the spot does not move it). `distance_m` and `side` apply to
  that resting position.

## 0.3.4 – 0.3.5

* Close follow no longer circles its spot (larger follow / catch-up radii, no 8-second re-issue)
  and lets go while the body runs an action: `move_to`, `sit`, and walking over to `activate` or
  `mount` release the offset first and keep it released until they finish.
* The tool description steers agents toward `relaxed` unless the player asks for close follow.

## 0.3.3

* `look_at` is timed (`duration_s`, default 15 s; 0 = until changed) and pauses while the body
  walks away from what it looks at, or fights. Papyrus `SetLookAt` turns the whole body, so a
  follower looking at something behind it used to follow the player backwards.

## 0.3.2 (MCP 0.4.1)

* **Horses.** Ownership is judged for the body, not the player: the follower's own horse (owned by
  its NPC record or a faction it is in) is not a crime to ride. `activate` walks over first from
  up to 80 m and follows a target that wanders. New `mount` (finds the follower's horse) and
  `dismount`. Standing order `remount` (on by default): back on the horse after a fight while the
  player rides. `mount` events for the player and bodies; `inspect` shows `owner` / `yours`.
* **Sitting** gets up to 3 tries, re-walking to the seat, and waits for the animation; a late
  success arrives as `sat_down`.
* **Gestures** that do not play say why (`reason`: sitting, riding, weapon drawn, walking); a pose
  refused mid-walk stops the body and retries.
* `see` says when it returned the player's screen; `perspective` is an enum.
* Riders are no longer reported as `sitting` (the engine's riding state shares the sitting value).
* `set_follow_style` changes only the fields given (calling it without `mode` used to reset it).

## 0.3.1

* `inspect` on an actor could crash: `TESNPC::GetObjectVoiceType()` returned a misaligned,
  non-form pointer for some NPCs. The voice type is now read from the record's field and checked.
  Read-only perception (observe, nearby, inspect, inventory, quests, crosshair, map, locate, read,
  spells, dialogue_options) runs under a fault guard: a crash-class fault there becomes an error
  for the agent and an `error` line in the log (`… caught exception 0xc0000005 at SkyrimSE.exe+…`).

## 0.3.0 (MCP 0.4.0)

* `see` perspective="agent" pins the free camera's rendered position to the follower's eyes.
* `observe` names the nearest landmark outdoors (`location.near`).
* `sit` / `stand_up` on real furniture; `gestures` and poses through `emote`.
* `move_to` a load door stops in front of it.
* `spells` and `learn_spell` (from a tome in the follower's inventory).
* `wait_for_events` stays under host tool-call limits (~50 s).
* `animunculi-mcpl`: the server as [MCPL](https://github.com/anima-research/mcpl) 0.5, with pushed
  events, a chat channel, presence and thinking indicators.

## 0.2.x

* Subtitles from agent lines are removed properly (HUD hide message, speaker reset, a sweep for
  strays).
* `read` books and notes, `map`, `locate`, `dialogue_options`, keepsake screenshots (`see save`),
  standing orders (`set_combat`: style presets, restraint, medic; `set_follow_style`), `use_item`
  with a hand, importance filtering and the `animunculi-watch` background watcher.

## 0.1.x

* Fixed the crash on the first `observe`: `Actor::IsInBleedout` has no AE address in
  CommonLibSSE-NG 6.7.1. The build now runs `scripts/audit_relocations.py`, which fails if any
  linked CommonLib function lacks an SE or AE address.
* Item events are held briefly so scripted add/remove pairs cancel out, and say how the item moved.
* The body's own furniture use (sandboxing) is not reported as `activated`.
