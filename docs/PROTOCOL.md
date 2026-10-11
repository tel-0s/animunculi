# Animunculi wire protocol (v1)

The SKSE plugin (`Animunculi.dll`) runs a small TCP server inside the game. Anything that
speaks this protocol can drive an embodied agent; the bundled MCP server (`mcp/`) is one such client.

## Transport

* TCP, default `127.0.0.1:47700` (`[Server] BindAddress`/`Port` in `Animunculi.ini`).
* UTF-8, **one JSON object per line** (`\n`-terminated). Maximum line length is 48 MiB, which is
  enough for inline audio and images.
* Requests may be pipelined. Responses carry the request `id` and may arrive out of order.
* On startup the plugin writes a discovery file with the port, token, pid and versions:
  `Documents/My Games/Skyrim Special Edition/SKSE/Animunculi.discovery.json`.

```json
{"id": 7, "method": "observe", "params": {"agent": "default"}}
{"id": 7, "result": {"...": "..."}}
{"id": 8, "error": {"code": "not_bound", "message": "agent 'default' has no body"}}
```

## Authentication

The first request must be `hello`. Anything else is rejected with `unauthorized` and the connection
is closed.

```json
{"id": 0, "method": "hello", "params": {"token": "<from discovery file or INI>", "client": "animunculi-mcp/0.1.0", "protocol": 1}}
```

The result includes `plugin_version`, `game_version`, `runtime` (`SE`/`AE`), `protocol` and `event_seq`
(the sequence number the next event will get).

## Error codes

| code | meaning |
| --- | --- |
| `unauthorized` | missing/bad token |
| `bad_request` | malformed request or missing parameter |
| `unknown_method` | no such method |
| `not_in_game` | main menu, loading screen, or no player |
| `not_bound` | the agent id has no body |
| `not_loaded` | the body exists but is not in a loaded cell (no 3D) |
| `not_found` | a target could not be resolved |
| `disabled` | the feature is turned off in `Animunculi.ini` |
| `busy` | the game did not service the request in time (loading, heavy stall) |
| `failed` | the game refused the action |
| `internal` | an exception in the plugin |

## Targets

Anywhere a `target` or `item` string is accepted:

| form | meaning |
| --- | --- |
| `self` | the agent's own body |
| `player` | the player |
| `crosshair` | whatever the player is currently pointing at |
| `combat_target` | the agent's current combat target |
| `0x0001A696` | a reference FormID (as reported by `observe`/`nearby`), or a base form for items/spells |
| anything else | case-insensitive name match: nearby references (closest first), or inventory/spell names |

Positions are `[x, y, z]` in game units. All distances returned to clients are in **meters**
(1 m ≈ 70 units). Bearings are degrees, clockwise, relative to the agent's facing (0 = straight ahead).

## Methods

### Session
| method | params | result |
| --- | --- | --- |
| `hello` | `token`, `client`, `protocol` | versions, `event_seq` |
| `ping` | – | `{pong: true}` |
| `status` | – | `in_game`, `paused`, `menus`, `agents`, `player`, `game_time`, `features` |
| `agents` | – | `{agents: [{agent, bound, id, name, loaded}]}` |
| `bind` | `agent`, `target` | `{agent, id, name}`: embodies the agent in an actor |
| `unbind` | `agent` | `{}` |

### Perception
| method | params | result |
| --- | --- | --- |
| `observe` | `agent`, `radius_m` (40), `max_actors` (12), `max_objects` (12) | `time`, `location` (outdoors with `near`: the closest map marker), `self`, `player`, `actors`, `objects`, `speaking` |
| `nearby` | `agent`, `radius_m`, `kinds` (`actor`,`item`,`container`,`door`,`furniture`,`activator`,`flora`), `limit`, `los_only` | `{items: [...]}` |
| `inspect` | `agent`, `target` | a detailed description of one reference |
| `inventory` | `agent`, `who` (`self`/`player`/target), `filter`, `limit` | `{owner, gold, items: [...]}` |
| `quests` | – | active player quests with their displayed objectives |
| `crosshair` | – | what the player is pointing at (or `null`) |
| `read` | `agent`, `target` (book id/name in inventory or within 6 m; `open` = the book the player has open), `offset`, `max_chars` (6000) | `{id, title, kind, source, text, total_chars, offset, next_offset?}` |
| `map` | `agent`, `radius_km` (3), `limit` (12) | `here` (place, hold, map), `places` (discovered markers: type, direction, distance, bearing), `quest_targets`, `exits` (indoors) |
| `locate` | `agent`, `name` | `{matches: [{name, type, map, on_your_map, discovered, direction?, dist_km?, entrances? (separate regions)}]}` |
| `dialogue_options` | – | `{open, with, topics, last_chosen}`: the player's current dialogue choices (read-only) |

### Events
| method | params | result |
| --- | --- | --- |
| `events` | `since`, `limit` (100), `types` | `{events, next_seq, dropped}` |
| `wait_events` | `since`, `timeout_ms` (≤ 120000), `types`, `limit` | as `events`; returns as soon as one matching event exists |

Every event has `seq`, `type`, `t` (unix ms), and `game_hours` (in-game hours since game start).
Types: `heard` (NPC speech from subtitles or the dialogue menu), `player_chat` (typed text from the
chat window or Papyrus), `player_dialogue` (a topic the player picked in conversation),
`dialogue_started`, `dialogue_ended`, `combat`, `death`, `hit`, `bleedout`, `location_changed`,
`item_received`, `item_removed`, `quest_stage`, `level_up`, `spell_cast`, `activated`,
`speech_started`, `speech_finished`, `action_finished`, `agent_bound`, `agent_unbound`,
`agent_loaded`, `agent_unloaded`, `game_loaded`, `mount` (`who`, `state` mounted/dismounted, `horse`; the player and bodies), `sat_down` (a sit that reported failure took effect late), `health_low` (player or agent under 35%, once per
dip), `healed` (medic orders), `restrained` (restraint orders stopped a fight), `custom` (from
Papyrus `Animunculi.EmitEvent`).

`item_received` / `item_removed` carry `how`: `given_by_player`, `given`, `taken_from_container`,
`picked_up` or `spawned` (created by a script or the game); `given_to_player`, `given`, `stored`,
`dropped` or `removed` (consumed, destroyed or taken by a script). Item events are held for 1.5 s,
and an add/remove pair of the same item cancels out, so scripted blips (such as Nether's Follower
Framework refreshing a follower's 3D with a token ring on every load door) never reach the agent.
The body's own autonomous furniture use (sandboxing) is not reported as `activated`.

Filtering keeps the feed about the agent's surroundings. Combat, hits, spells and activations are
reported only when they involve the player or an agent body. Deaths are reported within 60 m of
the player. `heard` covers speakers within 45 m.

### Voice
| method | params | result |
| --- | --- | --- |
| `say` | `agent`, `text`, `audio_b64` or `audio_path` (WAV), `subtitle` (true), `interrupt` (false: queue behind the current line), `emotion`, `target` (look at) | `{speech_id, duration_ms, lipsync, audio, queued_behind_ms?}` |
| `stop_speaking` | `agent` | `{}` |

Audio must be RIFF WAV (PCM 8/16/24/32-bit, or 32-bit float; any rate; mono or stereo). It is played
positionally from the agent's head with amplitude-driven lip sync. Without audio, the subtitle is shown
for a reading-time duration.

### Vision
| method | params | result |
| --- | --- | --- |
| `capture` | `perspective` (`player`/`agent`), `max_dim` (1024), `format` (`jpeg`/`png`), `quality` (80), `save`, `caption` | `{mime, data_b64, width, height, perspective, pov_applied (agent only), saved_to?}` |

`perspective: "agent"` switches to the free camera (as Photo Mode does), holds it at the agent's
eyes for a few frames, captures, and switches back. The player sees a brief flicker. `pov_applied`
confirms that the rendering camera really was at the eyes. It is refused while menus or dialogue
are open. `save: true` also writes a full-resolution PNG keepsake (`[Vision] KeepsakeFolder`,
default `Documents/My Games/Skyrim Special Edition/Screenshots/Animunculi`).

### Action
All actions return quickly. Actions that take time return an `action_id`, and an `action_finished`
event follows later.

| method | params |
| --- | --- |
| `move_to` | `agent`, `target` or `position`, `run`, `stay` (wait there on arrival). A load-door target stops in front of the door (`door` in the result) |
| `follow` / `wait_here` | `agent` |
| `activate` | `agent`, `target`, `allow_crime`, `approach` (default true). Within 4.5 m (6 m for a horse) it happens at once; farther (up to 80 m) the body walks over first, chasing a target that wanders, and the result has an `action_id` (`action_finished` follows). A horse always reports through `action_finished` with `mounted`. Ownership is judged for the body: its own things (owned by its NPC record or a faction it is in) are not a crime |
| `mount` | `agent`, `target` (omitted: the horse it last rode, else a mount it owns within 80 m), `allow_crime` -> `action_id`; `action_finished` with `mounted` |
| `dismount` | `agent` (pauses the `remount` order until the player next gets off and on a horse) |
| `pick_up` | `agent`, `target`, `count`, `allow_theft` (within 4.5 m; the body's own things are not theft) |
| `give` | `agent`, `item`, `count`, `to` (`player`; within 8 m) |
| `use_item` / `unequip` | `agent`, `item`, `hand` (`left`/`right`, for spells and one-handed weapons) |
| `cast` | `agent`, `spell`, `target` |
| `attack` / `stop_combat` | `agent`, `target`, `force` (override restraint) |
| `look_at` | `agent`, `target` (omitted: look ahead), `duration_s` (15; 0 = until changed). Papyrus SetLookAt turns the whole body, so the gaze is paused while the body travels away from the target (more than 60 degrees off its heading) or fights, and resumes once it stands still |
| `emote` | `agent`, `expression`, `strength`, `duration_ms`, `animation` (a name from `gestures`, or a raw animation event) |
| `gestures` | - (curated vanilla idles: one-shots and poses that last until `stand`) |
| `sit` | `agent`, `target` (a seat, or the crate/log/bench it is on; omitted = nearest within 8 m), `pose` (`sit`/`lean`) -> `action_id`; `action_finished` says `sitting` (up to 3 tries, re-walking to the seat; a success within 90 s of a failure arrives as `sat_down`) |
| `stand` | `agent` (get up from furniture or out of a pose; resumes following) |
| `spells` | `agent`, `include_passive` -> known spells with cost, school, delivery, `heals_others`, equipped hand |
| `learn_spell` | `agent`, `tome` (a spell tome in the agent's inventory; consumed) |
| `draw_weapon` | `agent`, `draw` |
| `open_inventory` | `agent` |
| `notify` | `text` |
| `actions` | – (mod-registered actions) |
| `perform` | `agent`, `name`, `args` (object) |
| `console` | `command`, `target` (**disabled by default**) |

### Standing orders
Carried out by the plugin in real time and saved in the co-save.

| method | params |
| --- | --- |
| `set_combat` | `agent`, `style` (`default`, `ranged`, `magic`, `spellsword`, `melee`, `defensive`, `berserker`: vanilla combat styles, from the next fight), `restraint` (stand down against guards, unaggressive actors and allies), `medic` + `medic_threshold` (0.4: heal the player with a known heal-other spell) |
| `set_follow_style` | `agent`, `mode` (`close`: while the player travels the normal follower AI walks the body; once the player has stood still for 2 s it is held beside them with `KeepOffsetFromActor` and a wide radius, so it neither wanders off nor sandboxes, and does not move when the player only turns. Not during `move_to`, `sit`, approach-`activate`/`mount`, waiting, combat or riding. Holding the offset while travelling was dropped: the engine fixes the follower's facing when the call is issued, so after a turn she moonwalked; `relaxed`: normal follower AI), `distance_m` (1-8), `side` (`left`/`right`/`behind`), `remount` (default true: while the player rides and the body is not riding or fighting, it walks to its horse and gets on, up to 3 tries per ride). Omitted fields keep their current value. The close-follow offset is released while anyone rides |
| `orders` | `agent` → current orders |
| `presence` | `agent`, `thinking`: the host reports that the agent's model is thinking (chat-window indicator, `Animunculi_Thinking` mod event) |
