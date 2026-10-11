# Testing in game

A checklist for trying Animunculi with a follower, roughly from "surely works" to "most likely to
need a fix". Each row says where to look when it doesn't work. What changed between versions is in
[CHANGELOG.md](../CHANGELOG.md).

## Install

1. Install the mod archive (`dist/Animunculi-<version>.zip` from `.\build.ps1`) with your mod
   manager and enable it. Requirements: SKSE, Address Library for SKSE Plugins. Optional: SKSE Menu
   Framework, for the chat window and the bind menu.
2. Register the MCP server with your agent (see [mcp/README.md](../mcp/README.md)), e.g.
   `claude mcp add animunculi -- uv run --directory "<path-to-animunculi>/mcp" animunculi-mcp`
3. The log is `Documents/My Games/Skyrim Special Edition/SKSE/Animunculi.log`. A healthy start
   logs: the version, settings, `Papyrus natives registered`, `event sinks registered`,
   `frame hook installed`, `voice: XAudio2 ready`, `chat: window registered`,
   `vision: Present hooked`, `net: listening on 127.0.0.1:47700`, `discovery file written`.

## Basics

| # | Try | Expect | If not |
| --- | --- | --- | --- |
| 1 | Main menu: agent calls `game_status` | `in_game: false`, features listed | no connection → log `net:` lines; firewall prompts are not expected on loopback |
| 2 | Load a save, point at a follower, open SKSE Menu Framework → Animunculi → *Bind agent to crosshair target* (or agent: `bind_body`) | HUD "agent now inhabits <name>" | — |
| 3 | `observe` | sensible self/player/location/time/weather; bearings match (someone to the follower's left reads `left`); `location.near` outdoors | bearing sign flipped → `Describe::Placement` |
| 4 | Talk to a merchant while the agent runs `wait_for_events` | `player_dialogue` with your topic text, `heard` with their reply | nothing → `Events::PollDialogue` |
| 5 | Stand near ambient NPC conversations | `heard` events | try toggling General Subtitles |
| 6 | Press `\` in game, type, Enter | chat window; agent receives `player_chat` | hotkey clash → `[Chat] Hotkey` |
| 7 | `say` without TTS | subtitle over the follower and in the chat window; it disappears after the line | no subtitle → `[Voice] SubtitleMode = notification` |
| 8 | `say` with audio (`audio_path` to a WAV, or a TTS provider) | voice from the follower's position; lips move; lines queue | silent → log `voice:`; lips still → `LipSyncGain = 2` |
| 9 | `say` with `emotion: "happy"` and `to: "player"` | smiles, looks at you while speaking | — |
| 10 | `see` (no arguments) | the player's screen, with a `note` saying so | error → log `vision:` |
| 11 | `see` perspective="agent" | brief flicker; the view from the follower's eyes; `pov_applied: true` | `pov_applied: false` → send the `vision: agent view …` log line and the result's `diagnostic`; stuck in free camera → `tfc` in the console |
| 12 | Save, reload | binding and standing orders persist (`game_status` → `orders`) | co-save lines in the log |
| 13 | Leave the follower behind (wait, fast travel) | `agent_unloaded`; actions return `not_loaded` | — |

## Acting

| # | Try | Expect | If not |
| --- | --- | --- | --- |
| 14 | `move_to` a target, with and without `stay`; `follow_player`; `wait_here` | walks there; `action_finished`; waits or follows | `papyrus:` lines in the log |
| 15 | `move_to` a load door | stops in front of it (`door` note), does not go through | — |
| 16 | `activate` something 20 m away | walks over, then uses it; `action_finished` | `reason` in action_finished |
| 17 | `give_item` / `use_item` a potion / `pick_up` a loose item | inventory changes; `item_removed` / `item_received` | reach limits are deliberate (4.5 m pick up, 8 m give) |
| 18 | `use_item` a spell with hand="left" | the spell is in the left hand | — |
| 19 | `attack` a hostile; `cast_spell` a destruction spell | fights; magicka drops | — |
| 20 | `gestures`, then `emote` with `wave`, `sit_ground`, `lean_wall`, `dance`; then `stand_up` | each plays; poses hold until `stand_up` | `played: false` comes with a `reason` |
| 21 | `emote sit_ground` while seated, while walking, with a weapon out | `played: false` with a `reason`; while walking, a pose stops the follower and retries | — |
| 22 | `sit` on a bench, chair, log or camp crate, from a distance | walks over and sits (`action_finished` with `sitting: true`, `attempts` if it took more than one) | a late success arrives as `sat_down`; log `body: sit attempt …` |
| 23 | `look_at` something, then walk away from it | stops looking while walking (no walking backwards), looks again once stopped, eyes front after 15 s | — |
| 24 | Give a spell tome, `learn_spell`, then `spells` | learned (tome used up); listed with cost and school | — |

## Standing orders

| # | Try | Expect | If not |
| --- | --- | --- | --- |
| 25 | `set_combat` style="ranged" (with a bow) or "magic"; fight | keeps its distance in the next fight | — |
| 26 | `set_combat` restraint=true; hit a guard or a townsperson | stands down (`restrained` event) | — |
| 27 | `set_combat` medic=true (needs a healing spell such as Healing Hands); drop below 40% | heals you (`healed` event) | no spell → `health_low` still arrives |
| 28 | `set_follow_style` mode="close": walk, run, U-turns, stop and turn on the spot; then `sit` / `move_to` | follows normally while you move; 2 s after you stop it stays by your side (even as you turn) and does not wander or sit; `sit` and `move_to` still work | whether it misbehaves while moving or only after stopping |
| 29 | `wait_for_events` important_only=true, or `animunculi-watch --once` in the background | wakes for chat, fights, new places, loot, low health, being left behind; not for ambient chatter | — |

## Horses

| # | Try | Expect | If not |
| --- | --- | --- | --- |
| 30 | `inspect` the follower's horse; `activate` or `mount` it | `owner`, `yours: true`; riding it is allowed (not a crime) | the error names the owner; send it with what `inspect` says |
| 31 | `mount` with no target, the horse 20 m off | walks or runs to it, follows it if it moves, climbs on; `action_finished` with `mounted: true` | `reason` in action_finished |
| 32 | Fight while both are riding; after it ends | the follower gets back on within ~20 s (log `behavior: … remounting …`) | `set_follow_style` shows `remount` |
| 33 | `dismount` while the player rides | stays off until the player gets off and on again | — |

## Things to watch

* **Hitches.** A capture stalls for one frame by design. Anything else periodic is worth reporting.
* **Crash logs.** The PDB ships next to the DLL, so Crash Logger shows `Animunculi.dll` frames with
  names and source lines.
* **Subtitles** that stay after a line ends → `SubtitleMode = notification`, and send the
  `voice: subtitle removed …` log lines.

## Compatibility notes

* **Nether's Follower Framework** refreshes a follower's 3D on load doors by briefly adding and
  removing a ring (`nwsFF_silverRing`); Animunculi holds item events for 1.5 s so such pairs cancel
  out. NFF's sandboxing (after the player idles, or "Autobox" in towns) is what sends followers to
  every bench: raise its Relax Idle Delay, or use `set_follow_style` mode="close".
* **Talking to NPCs** on the follower's own initiative is not possible through Skyrim's dialogue
  system, which belongs to the player. The agent sees the player's current options
  (`dialogue_options`) and hears the replies.
