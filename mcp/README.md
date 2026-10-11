# animunculi-mcp / animunculi-mcpl

An MCP server that lets an AI agent inhabit a Skyrim character through the **Animunculi** SKSE plugin.
Through it the agent can perceive the world, hear what's said nearby, speak with a voice and lip sync,
see the game (if the model supports images), and act.

It connects to the plugin over `127.0.0.1:47700`. The port and a per-session token are discovered
automatically from `Documents/My Games/Skyrim Special Edition/SKSE/Animunculi.discovery.json`.

## Hooking it up

Claude Code:

```powershell
claude mcp add animunculi -- uv run --directory "<path-to-animunculi>/mcp" animunculi-mcp
```

Any MCP client that takes a JSON config (Claude Desktop, etc.):

```json
{
  "mcpServers": {
    "animunculi": {
      "command": "uv",
      "args": ["run", "--directory", "<path-to-animunculi>/mcp", "animunculi-mcp"],
      "env": { "ANIMUNCULI_TTS": "none" }
    }
  }
}
```

The built wheel works too: `uvx --from dist/animunculi_mcp-0.1.0-py3-none-any.whl animunculi-mcp`.

Then ask the agent to use the `embody` prompt, or just tell it to play. A good loop is:
`game_status`, then `observe`, then repeatedly `wait_for_events` → react (`say`, `move_to`, ...).

## Tools

| Tool | What it does |
| --- | --- |
| `game_status` | connection, versions, menus, bound body |
| `bind_body` | pick which actor the agent inhabits (`crosshair` by default) |
| `observe` | self, player, place, time, weather, nearby actors and objects, recent events |
| `nearby` / `inspect` | filtered listing / details of one thing |
| `inventory` / `quest_log` | items (self, player, a nearby container) / active quest objectives |
| `read` | the text of a book or note: in your inventory, within reach, or the one the player has open |
| `map` / `locate` | where you are, discovered places around you, quest-marker directions, exits; find any named place |
| `dialogue_options` | the topics the player can choose in their current conversation |
| `wait_for_events` | long-poll: player chat, overheard speech, combat, deaths, items handed over... (`important_only` sleeps through the noise) |
| `say` / `stop_speaking` | speak with subtitles, positional voice audio and lip sync; lines queue |
| `see` | the player's view, or the agent's own eyes (free camera, brief flicker); `save` keeps a full-res PNG |
| `move_to`, `follow_player`, `wait_here` | movement (the follower AI handles pathing and combat) |
| `activate`, `pick_up`, `give_item`, `use_item`, `unequip_item` | interaction and items (`use_item` takes a `hand`) |
| `cast_spell`, `attack`, `stop_combat`, `ready_weapon` | combat |
| `look_at`, `emote`, `gestures` | head tracking, facial expressions, gestures and poses (a curated list) |
| `sit`, `stand_up` | sit or lean on real furniture (benches, chairs, log/crate seats, rails) and get up |
| `mount`, `dismount` | get on your own horse (found for you) or off it; `remount` in set_follow_style gets you back on after a fight |
| `spells`, `learn_spell` | known spells with costs and hands; learn from a tome you were given |
| `open_inventory`, `notify_player` | trade window; out-of-character HUD notices |
| `set_combat` | standing orders: combat style preset (ranged/keep distance, magic, melee, ...), restraint, medic |
| `set_follow_style` | `close` (stay beside the player, no benches) or `relaxed` (normal follower AI); `remount` |
| `list_mod_actions`, `perform_mod_action` | verbs that follower mods register via Papyrus |
| `console_command` | debug only (`ANIMUNCULI_DEBUG_TOOLS=1` **and** `[Debug] AllowConsole=true`) |

## MCPL: the game reaches out (`animunculi-mcpl`)

`animunculi-mcpl` is the same server spoken as [MCPL](https://github.com/anima-research/mcpl)
0.5, a backward-compatible MCP extension. Plain MCP hosts see the usual tools. MCPL hosts
additionally get five feature sets, each available only if the host grants the capabilities it
`uses`:

| Feature set | Uses | What the host gets |
| --- | --- | --- |
| `skyrim.events` | `pushEvents` | `push/event` when something matters: being spoken to, fights, arrivals, loot, someone badly hurt or down, being left behind. Tagged `animunculi:<kind>` and `animunculi:priority=high/normal/low`, plus `chat:dm`/`chat:addressed` when the player addresses the agent |
| `skyrim.chat` | `channels.register`, `.incoming`, `.publish` | channel `skyrim:<agent>`: the player's typed lines arrive as `channels/incoming` (`chat:dm`, `chat:from-human`); text **published to the channel is spoken aloud in-game** by the body (TTS, lip sync, subtitles) |
| `skyrim.chat.overhear` | `+ channels.lifecycle` | once the host opens the channel, nearby NPC speech and the player's conversations with others arrive as `chat:ambient`; `channels/open` with `history` returns recent lines |
| `skyrim.presence` | `contextHooks.beforeInference.inject.beforeUser` | a one-line `[Skyrim · time] You (Lydia): 100% health · at … · player 2 m ahead · near: …` before every turn. It is inject-only: `observe` is never advertised, so the user's message is never read (SPEC §15.2) |
| `skyrim.thinking` | `inferenceLifecycle` | "Lydia is thinking..." in the in-game chat window while a turn runs, and an `Animunculi_Thinking` mod event for follower mods |

The server follows SPEC 0.5 negotiation. Nothing privileged happens before the host's initial
`featureSets/update` Request is answered. The grant is the only allowlist (`*` matches one
segment). A Notification can only reduce. Every outbound use is checked against the current
grant, and the degradation receipt says what each missing capability costs.

By default only wake-worthy events are pushed. `ANIMUNCULI_PUSH=all` also pushes everything else
tagged `animunculi:priority=low`, and the tag ontology suggests muting those.

### In Claude Code, via [mcpl-cc-bridge](https://github.com/anima-research/mcpl-cc-bridge)

Install the bridge plugin, then copy [`mcpl-bridge.example.json`](mcpl-bridge.example.json) to
`<project>/.mcpl-bridge.json` or `~/.claude/mcpl-bridge/config.json`, and start Claude Code with
the channel lane enabled:

```powershell
claude --dangerously-load-development-channels plugin:mcpl-bridge@mcpl-bridge-dev
```

The session now wakes on its own when the player types, a fight starts, and so on.
`"wake": "chat"` holds overheard NPC speech and folds it into the next wake instead of starting
a turn per line. The agent replies to the player with `mcpl_send` on channel `skyrim:default`,
which speaks the reply aloud in game. The `animunculi__*` tools are the same as the plain MCP
server's. Use this *instead of* registering `animunculi-mcp` directly, not alongside it.

### In connectome-host (agent-framework)

Add the server with its feature sets enabled, and give push events from `animunculi` a wake
policy:

```jsonc
"mcpServers": {
  "animunculi": {
    "command": "uv",
    "args": ["run", "--directory", "<path-to-animunculi>/mcp", "animunculi-mcpl"],
    "enabledFeatureSets": ["skyrim.*"]
  }
},
"modules": { "wake": { "policies": [
  { "name": "animunculi-wake", "match": { "scope": ["mcpl:push-event"], "source": "animunculi" }, "behavior": "always" }
] } }
```

(`origin.source` is `animunculi` on every push. The format is the one heartbeat-mcpl's README
documents; check it against your connectome-host version.)

### Checked against a real host

`tests/conformance/cc_bridge_host.ts` runs the bridge's own `McplServerHandle` against
`animunculi-mcpl` and the mock game. It covers the handshake, grant, feature-set derivation,
channel registration, tagged chat and push deliveries, the presence hook, speaking on publish,
and ambient speech after `channels/open`. `build.ps1` runs it when bun and a bridge checkout are
present (`MCPL_BRIDGE_DIR`).

## Waking up on its own without MCPL: `animunculi-watch`

MCP calls only happen when the agent makes them, so an agent between turns can't notice that
someone attacked, spoke to it, or handed it something. If the host can run a command in the
background and wake the agent when it exits (Claude Code's background Bash does), arm the watcher:

```powershell
uv run --directory "<path-to-animunculi>/mcp" animunculi-watch --once --timeout 900
```

It exits as soon as something important happens: the player speaks to the agent, a fight starts,
it arrives somewhere new, loot is received, someone is badly hurt or down, the agent is left
behind, or a mod event arrives. It prints the events, each with a `why`, plus counts of anything
else that happened. Re-arm it after reacting. Without `--once` it streams one JSON line per burst,
for line-based monitors. It uses its own connection, so the MCP session's events are untouched.

## Configuration

Every setting is an environment variable. You can also put them in a JSON file named by
`ANIMUNCULI_CONFIG`, using the lower-case name without the prefix.

| Variable | Default | Meaning |
| --- | --- | --- |
| `ANIMUNCULI_AGENT` | `default` | which embodied agent this MCP session drives |
| `ANIMUNCULI_HOST` / `_PORT` / `_TOKEN` | discovery file | override the connection (e.g. game on another PC) |
| `ANIMUNCULI_DISCOVERY` | Documents/My Games/... | explicit discovery file path |
| `ANIMUNCULI_TTS` | `none` | `none`, `command`, or `openai` |
| `ANIMUNCULI_TTS_COMMAND` | – | shell command that writes a WAV to `{out}`; the text comes in on stdin (also in `{text_file}`); `{voice}` is available |
| `ANIMUNCULI_TTS_URL` | `http://127.0.0.1:8880/v1/audio/speech` | OpenAI-compatible speech endpoint |
| `ANIMUNCULI_TTS_MODEL` / `_VOICE` / `_API_KEY` | `tts-1` / `alloy` / – | request fields |
| `ANIMUNCULI_FFMPEG` | `ffmpeg` | converts non-WAV audio (MP3/OGG/...) |
| `ANIMUNCULI_DEBUG_TOOLS` | `0` | expose `console_command` |

Voice examples:

```powershell
# Piper (local, fast): text arrives on stdin
$env:ANIMUNCULI_TTS = "command"
$env:ANIMUNCULI_TTS_COMMAND = 'piper --model C:\voices\en_GB-alba-medium.onnx --output_file "{out}"'

# Kokoro-FastAPI / openedai-speech / OpenAI
$env:ANIMUNCULI_TTS = "openai"
$env:ANIMUNCULI_TTS_URL = "http://127.0.0.1:8880/v1/audio/speech"
$env:ANIMUNCULI_TTS_VOICE = "bf_emma"
```

Audio made elsewhere can be passed per line: `say(text=..., audio_path="C:/lines/0042.wav")`.

## Developing without Skyrim

```powershell
uv run animunculi-mock-game --port 47700 --token test     # fake game speaking the real protocol
$env:ANIMUNCULI_PORT = 47700; $env:ANIMUNCULI_TOKEN = "test"; uv run animunculi-mcp
uv run --extra dev pytest -q                               # tests (mock game + real C++ server)
```
