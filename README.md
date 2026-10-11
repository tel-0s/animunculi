# Animunculi

*Embodied AI agents for Skyrim SE/AE.* An SKSE plugin plus an MCP server. Together they let an AI
agent inhabit a Skyrim character (typically a follower) and play alongside you. The agent perceives
its surroundings, hears conversations, talks with a positional voice and lip sync, sees the game,
and acts. Skyrim's own follower AI keeps handling real-time movement and combat.

> In Dwemer lore, *animunculi* are constructs animated by something bound into them from elsewhere.
> Here the follower AI is the clockwork and the agent is the bound mind. See [docs/DESIGN.md](docs/DESIGN.md).

| Part | Where | What |
| --- | --- | --- |
| SKSE plugin | `src/` → `Animunculi.dll` | in-game TCP server; perception, events, voice, vision, actions, chat window |
| Papyrus API | `data/Scripts/Source/Animunculi.psc` | for follower mods: bind a body, forward player text, register custom actions |
| MCP server | `mcp/` (Python) | the agent-facing tools, TTS integration, a mock game for development |
| Protocol | [docs/PROTOCOL.md](docs/PROTOCOL.md) | JSON lines over loopback TCP with a token; any client can use it |

Targets Skyrim SE 1.6.1170 and later runtimes through Address Library (1.5.97 as well). It uses no
version-specific offsets. VR is not supported.

## Quick start

```powershell
.\build.ps1                                  # build, run native + MCP tests, compile Papyrus, package into dist\
.\build.ps1 -Deploy -DeployTo <mods>\Animunculi  # ...and copy the mod into your mod manager
claude mcp add animunculi -- uv run --directory "<path-to-animunculi>/mcp" animunculi-mcp
```

In game, point at a follower. Then either use SKSE Menu Framework → *Animunculi* → *Bind agent to
crosshair target*, or have the agent call `bind_body`. Press `\` to type to your companion.
The in-game checklist is in [docs/PLAYTEST.md](docs/PLAYTEST.md); what changed is in
[CHANGELOG.md](CHANGELOG.md).

## Layout

```
src/core     WAV decoding, lip-sync analysis, event log, text utils (game-independent, unit-tested)
src/net      JSON-lines TCP server + RPC dispatch (game-independent, unit-tested)
src/game     CommonLibSSE-NG integration: agents, targets, describe, events, voice, vision, actions,
             Papyrus natives, chat window, method table
tests/native native tests (+ `--serve` headless mode used by the Python cross-language tests)
data/        INI and Papyrus sources/compiled script shipped with the mod
mcp/         animunculi-mcp (Python, `mcp` 2.x) + mock game + tests
scripts/     Papyrus compile and packaging
```

## Building

Requirements: Windows, Visual Studio 2026 (C++ workload), CMake 3.24+, Python 3.11+ and
[uv](https://docs.astral.sh/uv/). Optional: the Creation Kit's Papyrus compiler (otherwise the
committed `Animunculi.pex` is packaged), and [bun](https://bun.sh) plus an
[mcpl-cc-bridge](https://github.com/anima-research/mcpl-cc-bridge) checkout for the MCPL
conformance test.

C++ dependencies (CommonLibSSE-NG 6.7.1 prebuilt, spdlog, nlohmann-json, stb) are taken from a
[skyrim-community-shaders](https://github.com/doodlum/skyrim-community-shaders) checkout that has
been built once. Point `ANIMUNCULI_CS_ROOT` at it (or pass `-DCS_ROOT=...` to CMake). Machine
settings go in a git-ignored `build.local.ps1`; copy
[build.local.example.ps1](build.local.example.ps1) to start.

The MCP server alone needs only Python and uv: see [mcp/README.md](mcp/README.md).
