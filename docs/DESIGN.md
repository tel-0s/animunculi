# Animunculi: design

## The name

In Dwemer lore, **animunculi** are the constructs the Dwarves built: bodies that carry out
complex behaviour, animated by something bound into them from elsewhere. That is the arrangement
here. Skyrim's follower AI is the clockwork: it handles pathing, combat, keeping up and sandboxing
in real time. The agent is the bound mind. It perceives, talks and decides, and it steers the body
without having to puppeteer every step. (CHIM and Mantella are taken; Mantella, the Numidium's
heart, is fittingly next door in the lore.)

## Shape

```
 MCP client (Claude, ...)          animunculi-mcp (Python)            Skyrim + Animunculi.dll
 ─────────────────────────  stdio  ─────────────────────────  TCP   ───────────────────────────────
  tools: observe, say, see, ... ──► tool → protocol call ────────► net thread → RPC dispatch
                                    TTS / audio sourcing             │  worker: events, capture, say decode
                                    event cursor per agent           │  main:   SKSE task → game state
                                                                     ├─ Events: sinks + polled speech → ring buffer
                                                                     ├─ Voice: XAudio2 3D + lip sync + subtitles
                                                                     ├─ Vision: Present hook → JPEG/PNG
                                                                     ├─ Actions: equip, cast, path (via Papyrus VM)
                                                                     ├─ Papyrus API: bind, chat, mod actions
                                                                     └─ Chat window (SKSE Menu Framework)
```

* **Plugin ↔ MCP split.** The plugin speaks a small, documented JSON-lines protocol
  ([PROTOCOL.md](PROTOCOL.md)) and has no dependency on MCP. The MCP server is one client among
  potentially many (a custom agent loop, a web UI, a second agent). The plugin stays stable while
  the agent side evolves quickly in Python.
* **Threading.** Socket I/O and per-request workers run off the main thread. Anything that touches
  game state is marshalled onto the main thread through the SKSE task queue and awaited with a
  timeout. The timeout returns `busy` during loading screens instead of hanging. Per-frame work
  (lip sync, 3D audio, overheard-speech polling) runs in a `PlayerCharacter::Update` vtable hook.
  That hook only fires while the game is unpaused, and the audio thread uses the same signal to
  pause voices.
* **Version independence.** There are no hard-coded offsets. The plugin uses CommonLibSSE-NG with
  Address Library IDs for singletons and vtables, COM vtables for DXGI, the Papyrus VM for engine
  behaviours with a stable scripting contract (`PathToReference`, `SetLookAt`, `OpenInventory`), and
  SKSE event sources. It is declared `VersionIndependence::AddressLibrary`, so it loads on 1.6.1170
  and later runtimes that Address Library covers (and 1.5.97). VR is refused at load.

## Perception

`observe` is the workhorse. It returns the agent's body (health/magicka/stamina, equipment, combat
state, waiting), the player, the place (cell, location chain, location types such as `Inn` or
`Dungeon`, cleared state, weather), the time, and nearby actors and notable objects. Everything is
in meters and in bearings relative to the body's facing ("ahead-left", 3.4 m, 1.2 m up). Those are
the units a language model reasons in best. Only notable kinds are listed: actors, loose items,
containers, doors (with their destination), furniture, named activators and flora. Ownership and
locks are flagged so the agent can avoid committing crimes on the player's behalf.

`[Perception] RequireLineOfSight` restricts perception to what the body can actually see.
Container contents are only readable within 6 m. Embodiment is a feature: the agent should know
roughly what a companion would know.

### Hearing

There is no single "someone said something" event in Skyrim, so speech is gathered two ways:

1. **Subtitles.** The `SubtitleManager` array is polled at 15 Hz. It captures ambient and scene
   dialogue near the player, with the speaker.
2. **The dialogue menu.** While `DialogueMenu` is open, `MenuTopicManager::lastSelectedDialogue`
   is polled. It gives the topic the *player chose* (`player_dialogue`, i.e. what the player said)
   and the NPC's response lines (`heard`, `to_player: true`).

Both are de-duplicated by speaker and text, so a companion follows conversations the player has
with other NPCs.

### Player → agent

* **Chat window.** A hotkey (backslash) opens an SKSE Menu Framework window with the conversation
  and an input line. What you type becomes `player_chat`. The Menu Framework's own menu also gets
  an Animunculi page with "bind to crosshair target", which is handy before a follower mod exists.
* **Papyrus.** `Animunculi.PlayerSay(text)` lets a follower mod forward dialogue choices, UIExtensions
  text input, or a speech-to-text bridge.
* **Vanilla dialogue** with the body is reported as `dialogue_started`, plus `player_dialogue` lines.

## Voice

Skyrim's own dialogue pipeline needs pre-authored topics and `.fuz`/`.lip` files on disk. CHIM and
Mantella overwrite placeholder files to work around this. Animunculi plays audio itself instead:

* **XAudio2 2.9 + X3DAudio.** The mono voice is spatialised from the actor's head node relative to
  the camera. A linear falloff runs between `FullVolumeDistanceMeters` and `SilentDistanceMeters`,
  scaled by the game's master volume. Audio arrives inline (base64 WAV) or as a local WAV path, so
  there are no file-system tricks and no MO2 VFS problems, and the game can run on a different PC
  from the agent.
* **Lip sync from the waveform.** The clip is analysed up front at 60 Hz for loudness (jaw
  opening), a spectral-brightness proxy (spread "ee/eh" vs rounded "oh/oo"), and lip-closure pulses
  before syllable onsets. It is mapped onto FaceGen's phoneme morphs (Aah, BigAah, BMP, Eee, Eh, Oh,
  OohQ) each frame. This is not phoneme-accurate, but it is synchronised, works with any TTS, and
  needs no `.lip` generation step.
* **Subtitles** are injected into the `SubtitleManager` (standard presentation). If the game evicts
  them, the plugin falls back to HUD notifications. The mode is configurable.
* **Emotion** maps to the expression override for the duration of the line. Head tracking turns
  toward the addressee. Lines queue unless `interrupt` is set.
* Follower mods get `Animunculi_SpeechStarted/Finished` mod events, e.g. to play talking idles.

TTS stays **outside** the plugin, in the MCP server (`none`, `command` such as Piper, or any
OpenAI-compatible `/v1/audio/speech` such as Kokoro-FastAPI). Agents can also pass their own clips.

## Vision

* **Player view** (reliable): a vtable hook on the game's `IDXGISwapChain::Present` copies the back
  buffer to a staging texture on request (one-frame stall). It handles RGBA8/BGRA8/RGB10A2/FP16
  (HDR is tonemapped) and MSAA. It is downscaled (sRGB-correct) and encoded as JPEG/PNG off-thread.
  Only frames that are actually requested are touched.
* **Agent view**: a short free-camera session, the same mechanism Photo Mode uses.
  `PlayerCamera::ToggleFreeCameraMode` enters it. Each gameplay frame the plugin writes
  `FreeCameraState::translation` (just in front of the agent's head node) and `rotation`
  (pitch, yaw). These use the same angle convention as the actor, so no axis guesswork is needed.
  After a few frames it grabs one frame and toggles back. `pov_applied` compares the rendered
  camera position with the intended one. A one-time log line records the camera basis for
  calibration. (0.1.x moved the camera root inside a `PlayerCamera::Update` vtable hook, and the
  engine ignored it, presumably because the call is devirtualized.)
* **Keepsakes**: `save` writes the captured frame at full resolution as a PNG to
  `Screenshots/Animunculi`.

## Knowledge

* **Books**: `TESDescription::GetDescription` on the book form (the same text the book menu
  shows), cleaned of markup: `<p>`, `<br>`, `<font>`, `<img>`, `[pagebreak]`, entities and alias
  tokens. It comes back paged. The agent can read books it carries, books within 6 m, or the book
  the player has open (`BookMenu::GetTargetForm`).
* **Map**: map markers come from each worldspace's persistent cell (`ExtraMapMarker`). Only markers
  on the player's map are listed as nearby places; `locate` also finds undiscovered ones and says
  so. Indoors, positions are anchored to the enclosing location's world marker, which is how the
  game's map places you. Quest targets are the resolved `TESQuestTarget`s of displayed objectives.
  A separate region such as Blackreach is reported with its entrances: the destinations of its
  load doors.
* **Dialogue**: the player's open topic list (`MenuTopicManager::dialogueList`) is readable, so the
  agent can suggest what to ask. Choosing is left to the player, since Skyrim has no engine path for
  a follower to converse with an NPC.

## Standing orders

Some behaviour has to happen between the agent's turns: in a fight, a tool round-trip is far too
slow. So the agent sets orders, and the plugin carries them out every frame. Orders are saved in
the co-save.

* **Combat style presets** swap the body's `TESNPC::combatStyle` for a vanilla style (stable
  Skyrim.esm IDs: `csHumanMissile` keeps its distance, `csHumanMagic`, `csWESpellsword`, ...). The
  original is remembered for `default`, and the preset is re-applied after load, when base forms
  revert.
* **Restraint** checks the body's combat target four times a second. Guards, allies and
  unaggressive actors (Aggression 0) get `StopCombat` and a `restrained` event. `attack` refuses
  them too unless `force` is set.
* **Medic** checks the player's health twice a second. Below the threshold, the body casts its
  strongest affordable heal-other spell (Health value-modifier, not hostile, targeted or area),
  with magicka paid and a cooldown.
* **Follow style `close`** uses Papyrus `KeepOffsetFromActor` on the player while not in combat
  and not waiting. It is released automatically for fights and `wait_here`. This stops
  NFF/vanilla sandboxing (the bench problem) without touching the follower framework's settings.
* **`health_low`** events fire once per dip below 35% for the player and each body.

The MCP-side **importance filter** (`wait_for_events(important_only)`, `animunculi-watch`) decides
what wakes an agent. Being spoken to, fights, new places, loot, someone badly hurt or down, and
being left behind all count. Ambient chatter, hits and spell casts are summarised as counts.

## Actions

Actions are what a companion could plausibly do. Checks enforce reach: 4.5 m to activate or pick up,
8 m to hand over. Owned objects and theft are refused unless explicitly insisted on. Spells cost
magicka. Attacking the player is off by default.

| Verb | Mechanism |
| --- | --- |
| move_to | Papyrus `Actor.PathToReference` (latent; completion → `action_finished`), XMarker for coordinates; `stay` sets `WaitingForPlayer` on arrival |
| follow / wait | vanilla `WaitingForPlayer` actor value + package re-evaluation (works with vanilla and most frameworks) |
| activate / pick up / give | `ActivateRef`, `PickUpObject`, `RemoveItem(→ target)` |
| use / unequip | `ActorEquipManager` (equip, consume potions/food, equip known spells) |
| cast | `MagicCaster::CastSpellImmediate` with the magicka cost deducted |
| attack / stop | `StartCombat` / `StopCombat` |
| look at / emote | Papyrus `SetLookAt`; expression overrides; animation-graph events |
| mod actions | follower mods register verbs; invocation sends `Animunculi_Action_<name>`, and `CompleteAction` replies |

## The follower-mod contract (next step)

A follower mod built on Animunculi mostly needs Papyrus:

```papyrus
Event OnInit()
    Animunculi.Bind(Self as Actor, "default")
    Animunculi.RegisterAction("guard_door", "Stand guard at a door", "{\"door\":\"id\"}")
    RegisterForModEvent("Animunculi_Action_guard_door", "OnGuardDoor")
EndEvent
```

Its value-add is the body, voice type and personality, custom packages (guard, scout, stay
close), and dialogue hooks such as "I want to tell you something…" → `PlayerSay`. Bindings live
in the co-save, so re-binding on load is only needed if the mod swaps bodies.

## MCPL (0.3 / plugin 0.2.1)

An MCP tool server only speaks when spoken to, and embodiment is the opposite case: the world
happens to the agent. `animunculi-mcpl` speaks [MCPL](https://github.com/anima-research/mcpl) 0.5
so the game can reach the agent:

* **Wake-worthy events become `push/event`s** carrying producer tags (`animunculi:combat`,
  `animunculi:priority=high`, ...) and `chat:*` tags where they apply. The host's wake policy
  decides whether an event wakes the model, is held, or is muted. The server only decides what is
  worth offering (the importance filter).
* **The player/agent conversation is a channel.** Typed chat arrives as `chat:dm`. Publishing to
  the channel is the agent's voice: the reply is spoken aloud in game. Opening the channel admits
  overheard speech as `chat:ambient`, which chat-aware hosts hold rather than wake on.
* **Presence is write-without-read** (the §15.2 embodiment pattern). A `beforeUser` status line is
  injected each turn, and `observe` is not advertised, so the host never sends the user's message.
* **Turn lifecycle** drives an in-game "thinking" indicator, since LLM latency is visible to a
  player standing next to the companion.

It is a Python implementation of the spec's negotiation rules, ported from the semantics of
`mcpl-core-ts` and heartbeat-mcpl, and served over newline-delimited JSON-RPC on stdio. It reuses
the same tool registry as the plain MCP server, so there is one set of tools and one test suite.
It is verified against the Claude Code bridge's real host implementation.

## Security

* Loopback by default, with a random 128-bit token per session, published only in the discovery
  file in your Documents folder. The first request must authenticate. Tokens are compared in
  constant time.
* `console` (arbitrary console commands) is off by default and double-gated (plugin INI + MCP env).
* Text from the game world (names, books, NPC lines, mod-provided action descriptions) reaches the
  model. Treat it as untrusted input; the MCP instructions frame it as world content, not commands.

## Known limits / next

* Untested in-game as of 0.2.0: the free-camera agent view, book text retrieval, quest-target
  resolution, `KeepOffsetFromActor` follow, the medic and restraint loops. Each has a narrow
  failure mode and a log trail (see [PLAYTEST.md](PLAYTEST.md)).
* Lip sync is amplitude/brightness-driven. A future option could run a phoneme aligner (e.g. from
  TTS timestamps) through the same phoneme channel.
* Speech input (microphone → STT) belongs in the agent host. A PTT hotkey event would be cheap to add.
* "Go there and do X" chains are left to the agent. Packages authored in a follower mod (via
  `perform`) will beat `PathToReference` for complex behaviours.
