"""animunculi-watch: block until something important happens in Skyrim, then print it.

Made for agent hosts that can run a command in the background and get woken when it exits (for
example Claude Code's background Bash). Arm it, carry on, and when it returns, react and re-arm:

    animunculi-watch --once --timeout 900

Streaming mode (no --once) prints one JSON line per batch of important events and keeps going,
which suits line-by-line monitors. It uses its own connection and cursor, so the MCP server's
event stream is unaffected.

Exit codes: 0 = events printed, 3 = timed out with nothing important, 2 = error (e.g. bad token).
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
from typing import Callable

from .bridge import Bridge, BridgeError
from .config import Config
from .importance import split


async def watch(cfg: Config, *, once: bool, timeout_s: float, include_all: bool = False,
                emit: Callable[[str], None] = print, gather_s: float = 1.5) -> int:
    bridge = Bridge(cfg)
    deadline = time.monotonic() + timeout_s if timeout_s > 0 else float("inf")
    cursor: int | None = None
    try:
        while (remaining := deadline - time.monotonic()) > 0:
            try:
                if cursor is None:
                    await bridge.ensure_connected()
                    cursor = int(bridge.hello.get("event_seq", 0))
                res = await bridge.call("wait_events", {"since": cursor, "limit": 100,
                                                        "timeout_ms": int(min(remaining, 60) * 1000)},
                                        timeout=min(remaining, 60) + 10)
            except BridgeError as exc:
                if exc.code != "disconnected" and exc.code != "timeout":
                    raise
                await asyncio.sleep(min(5.0, max(0.0, remaining)))  # game not up yet / restarting
                cursor = None if exc.code == "disconnected" else cursor
                continue
            cursor = res.get("next_seq", cursor)
            events = res.get("events", [])
            important, rest = (events, {}) if include_all else split(events, cfg.agent)
            if not important:
                continue
            # things tend to come in bursts (a fight starts with several events); collect briefly
            await asyncio.sleep(gather_s)
            more = await bridge.call("events", {"since": cursor, "limit": 100})
            cursor = more.get("next_seq", cursor)
            extra, extra_rest = (more.get("events", []), {}) if include_all else split(more.get("events", []), cfg.agent)
            important += extra
            for k, v in extra_rest.items():
                rest[k] = rest.get(k, 0) + v
            out = {"events": [{k: v for k, v in e.items() if k != "t"} for e in important]}
            if rest:
                out["also"] = rest
            emit(json.dumps(out, ensure_ascii=False, separators=(",", ":")))
            if once:
                return 0
        emit(json.dumps({"timeout": True}))
        return 3
    finally:
        await bridge.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--once", action="store_true", help="exit after the first batch of important events")
    ap.add_argument("--timeout", type=float, default=900.0, help="seconds to wait in total (0 = forever)")
    ap.add_argument("--all", action="store_true", help="report every event, not just important ones")
    ap.add_argument("--agent", default=None, help="agent id (default: ANIMUNCULI_AGENT or 'default')")
    args = ap.parse_args()
    cfg = Config.load()
    if args.agent:
        cfg.agent = args.agent
    try:
        code = asyncio.run(watch(cfg, once=args.once, timeout_s=args.timeout, include_all=args.all,
                                 emit=lambda line: print(line, flush=True)))
    except BridgeError as exc:
        print(json.dumps({"error": str(exc)}), flush=True)
        code = 2
    except KeyboardInterrupt:
        code = 130
    sys.exit(code)


if __name__ == "__main__":
    main()
