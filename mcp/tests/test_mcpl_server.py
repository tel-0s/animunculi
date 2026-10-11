"""The MCPL server end to end: this test plays the MCPL host, drives the real `animunculi-mcpl`
process over stdio, and uses the mock game as the Skyrim side."""
from __future__ import annotations

import asyncio
import json
import os
import sys

import pytest

from animunculi_mcp.mock_game import MockGame

FULL_GRANT = ["tools", "pushEvents", "inferenceLifecycle", "contextHooks.beforeInference.inject.beforeUser",
              "channels.register", "channels.lifecycle", "channels.publish", "channels.incoming"]


class Host:
    """A minimal MCPL host: answers the server's requests and records everything it receives."""

    def __init__(self, proc: asyncio.subprocess.Process):
        self.proc = proc
        self.next_id = 0
        self.pending: dict[int, asyncio.Future] = {}
        self.received: list[dict] = []
        self.arrived = asyncio.Event()
        self.reader = asyncio.create_task(self._read())

    async def _read(self) -> None:
        while line := await self.proc.stdout.readline():
            msg = json.loads(line)
            if "method" in msg:
                self.received.append(msg)
                self.arrived.set()
                if "id" in msg:
                    p = msg.get("params") or {}
                    if msg["method"] in ("channels/register",):
                        result = {"results": [{"id": c["id"], "accepted": True} for c in p.get("channels", [])]}
                    elif msg["method"] == "channels/incoming":
                        result = {"results": [{"messageId": m["messageId"], "accepted": True} for m in p.get("messages", [])]}
                    else:
                        result = {"accepted": True}
                    self._write({"jsonrpc": "2.0", "id": msg["id"], "result": result})
            elif msg.get("id") in self.pending:
                self.pending.pop(msg["id"]).set_result(msg)

    def _write(self, msg: dict) -> None:
        self.proc.stdin.write(json.dumps(msg).encode() + b"\n")

    async def request(self, method: str, params: dict | None = None, timeout: float = 10) -> dict:
        self.next_id += 1
        fut = asyncio.get_running_loop().create_future()
        self.pending[self.next_id] = fut
        self._write({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}})
        await self.proc.stdin.drain()
        return await asyncio.wait_for(fut, timeout)

    async def notify(self, method: str, params: dict) -> None:
        self._write({"jsonrpc": "2.0", "method": method, "params": params})
        await self.proc.stdin.drain()

    async def expect(self, method: str, pred=lambda m: True, timeout: float = 8) -> dict:
        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            for m in self.received:
                if m["method"] == method and pred(m):
                    self.received.remove(m)
                    return m
            remaining = deadline - asyncio.get_running_loop().time()
            if remaining <= 0:
                raise AssertionError(f"no {method} arrived; got {[m['method'] for m in self.received]}")
            self.arrived.clear()
            try:
                await asyncio.wait_for(self.arrived.wait(), remaining)
            except asyncio.TimeoutError:
                pass

    def none_of(self, method: str) -> bool:
        return not any(m["method"] == method for m in self.received)


async def _setup(grant: list[str] | None, enabled: list[str] | None = None):
    game = MockGame(token="t", chatter=0)
    srv = await asyncio.start_server(game.serve_client, "127.0.0.1", 0)
    port = srv.sockets[0].getsockname()[1]
    env = {**os.environ, "ANIMUNCULI_HOST": "127.0.0.1", "ANIMUNCULI_PORT": str(port), "ANIMUNCULI_TOKEN": "t",
           "PYTHONPATH": os.path.join(os.path.dirname(__file__), "..", "src")}
    proc = await asyncio.create_subprocess_exec(sys.executable, "-m", "animunculi_mcp.mcpl.server", env=env,
                                                stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                                                stderr=asyncio.subprocess.DEVNULL)
    host = Host(proc)
    init = await host.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {"experimental": {"mcpl": {"version": "0.5"}}},
                                             "clientInfo": {"name": "test-host", "version": "0"}})
    await host.notify("notifications/initialized", {})
    receipt = None
    if grant is not None:
        params: dict = {"effectiveCapabilities": grant}
        if enabled is not None:
            params["enabled"] = enabled
        receipt = (await host.request("featureSets/update", params))["result"]
    return game, srv, proc, host, init, receipt


async def _teardown(srv, proc):
    proc.stdin.close()
    try:
        await asyncio.wait_for(proc.wait(), 5)
    except asyncio.TimeoutError:
        proc.kill()
    srv.close()


def test_advertises_write_without_read_embodiment():
    async def main():
        game, srv, proc, host, init, _ = await _setup(None)
        await _teardown(srv, proc)
        return init
    init = asyncio.run(main())["result"]
    mcpl = init["capabilities"]["experimental"]["mcpl"]
    assert mcpl["version"] == "0.5" and mcpl["pushEvents"] is True
    assert mcpl["contextHooks"]["beforeInference"]["observe"] is False  # never reads the user's message
    assert mcpl["contextHooks"]["beforeInference"]["inject"]["beforeUser"] is True
    assert set(mcpl["featureSets"]) == {"skyrim.events", "skyrim.chat", "skyrim.chat.overhear", "skyrim.presence", "skyrim.thinking"}
    assert init["protocolVersion"] == "2025-06-18"


def test_plain_mcp_host_gets_tools_but_nothing_privileged():
    async def main():
        game, srv, proc, host, _, _ = await _setup(None)
        tools = (await host.request("tools/list"))["result"]["tools"]
        obs = (await host.request("tools/call", {"name": "observe", "arguments": {}}))["result"]
        await game.emit("combat", state="combat", actor={"name": "Bandit"})
        await asyncio.sleep(1.0)
        hook = (await host.request("context/beforeInference", {"inferenceId": "i1", "conversationId": "c", "turnIndex": 0,
                                                                "userMessage": None}))["result"]
        pub = await host.request("channels/publish", {"channelId": "skyrim:default", "content": [{"type": "text", "text": "hi"}]})
        quiet = host.none_of("push/event") and host.none_of("channels/register")
        await _teardown(srv, proc)
        return tools, obs, hook, pub, quiet
    tools, obs, hook, pub, quiet = asyncio.run(main())
    names = {t["name"] for t in tools}
    assert {"observe", "say", "see", "map", "read", "set_combat"} <= names and len(names) >= 30
    assert "Lydia" in obs["content"][0]["text"] and not obs.get("isError")
    assert hook["contextInjections"] == []
    assert pub["error"]["code"] == -32002  # §5.3: nothing privileged before policy
    assert quiet


def test_full_grant_chat_channel_push_and_presence():
    async def main():
        game, srv, proc, host, _, receipt = await _setup(FULL_GRANT, ["skyrim.*"])
        reg = await host.expect("channels/register")
        # the player types in the chat window -> a DM on the channel, not a push
        await game.emit("player_chat", text="Shall we find Urag?", source="chat_window")
        chat = await host.expect("channels/incoming")
        # a fight -> a tagged push event
        await game.emit("combat", state="combat", actor={"name": "Bandit"}, target={"name": "Lydia", "agent": "default"})
        push = await host.expect("push/event", lambda m: "combat" in json.dumps(m["params"]["tags"]))
        # ambient speech is not delivered until the host opens the channel
        await game.emit("heard", speaker={"id": "0x1", "name": "Urag gro-Shub"}, text="Quiet in the Arcanaeum!")
        await asyncio.sleep(0.8)
        before_open = host.none_of("channels/incoming")
        opened = (await host.request("channels/open", {"channelId": "skyrim:default", "type": "skyrim", "address": {},
                                                       "history": {"limit": 5}}))["result"]
        await game.emit("heard", speaker={"id": "0x1", "name": "Urag gro-Shub"}, text="Books are not for eating.")
        ambient = await host.expect("channels/incoming")
        # the agent replies on the channel -> spoken in game
        pub = (await host.request("channels/publish", {"channelId": "skyrim:default",
                                                       "content": [{"type": "text", "text": "Lead the way."}]}))["result"]
        said = [p for m, p in game.log if m == "say"]
        # presence line before a turn; thinking indicator on lifecycle
        hook = (await host.request("context/beforeInference", {"inferenceId": "i1", "conversationId": "c",
                                                                "turnIndex": 1, "userMessage": None}))["result"]
        await host.notify("inference/lifecycle", {"inferenceId": "i1", "phase": "started"})
        await asyncio.sleep(0.5)
        thinking_on = game.__dict__.get("thinking")
        await host.notify("inference/lifecycle", {"inferenceId": "i1", "phase": "completed"})
        await asyncio.sleep(0.5)
        thinking_off = game.__dict__.get("thinking")
        await _teardown(srv, proc)
        return receipt, reg, chat, push, before_open, opened, ambient, pub, said, hook, thinking_on, thinking_off
    (receipt, reg, chat, push, before_open, opened, ambient, pub, said, hook,
     thinking_on, thinking_off) = asyncio.run(main())
    assert receipt == {"accepted": True, "mode": "full", "unavailableFeatures": [], "notes": []}
    assert reg["params"]["channels"][0]["id"] == "skyrim:default"
    m = chat["params"]["messages"][0]
    assert m["content"][0]["text"] == "Shall we find Urag?" and "chat:dm" in m["tags"] and "chat:from-human" in m["tags"]
    p = push["params"]
    assert p["featureSet"] == "skyrim.events" and "animunculi:priority=high" in p["tags"]
    assert p["payload"]["content"][0]["text"].startswith("[Skyrim] Fight: Bandit against Lydia")
    assert before_open
    assert opened["channel"]["id"] == "skyrim:default" and opened["history"][0]["content"][0]["text"] == "Quiet in the Arcanaeum!"
    a = ambient["params"]["messages"][0]
    assert a["tags"] == ["chat:ambient"] and a["author"]["name"] == "Urag gro-Shub"
    assert pub["delivered"] is True and said[-1]["text"] == "Lead the way."
    inj = hook["contextInjections"][0]
    assert hook["featureSet"] == "skyrim.presence" and inj["position"] == "beforeUser" and inj["content"].startswith("[Skyrim")
    assert "Lydia" in inj["content"]
    assert thinking_on is True and thinking_off is False


def test_degraded_grant_is_honest_and_enforced():
    async def main():
        game, srv, proc, host, _, receipt = await _setup(["tools"])
        await game.emit("combat", state="combat", actor={"name": "Bandit"})
        await game.emit("player_chat", text="hello?")
        await asyncio.sleep(1.2)
        silent = host.none_of("push/event") and host.none_of("channels/incoming") and host.none_of("channels/register")
        hook = (await host.request("context/beforeInference", {"inferenceId": "i", "conversationId": "c", "turnIndex": 0,
                                                                "userMessage": None}))["result"]
        pub = await host.request("channels/publish", {"channelId": "skyrim:default", "content": "hi"})
        tool = (await host.request("tools/call", {"name": "say", "arguments": {"text": "still works"}}))["result"]
        await _teardown(srv, proc)
        return receipt, silent, hook, pub, tool
    receipt, silent, hook, pub, tool = asyncio.run(main())
    assert receipt["accepted"] is True and receipt["mode"] == "degraded"
    assert {u["featureSet"] for u in receipt["unavailableFeatures"]} == {
        "skyrim.events", "skyrim.chat", "skyrim.chat.overhear", "skyrim.presence", "skyrim.thinking"}
    assert silent and hook["contextInjections"] == [] and pub["error"]["code"] == -32002
    assert not tool.get("isError")  # tools answer over plain MCP regardless


def test_malformed_policy_and_notification_reduction():
    async def main():
        game, srv, proc, host, _, receipt = await _setup(FULL_GRANT)
        await host.expect("channels/register")
        bad = await host.request("featureSets/update", {"effectiveCapabilities": ["pushEvents"], "deniedCapabilities": ["pushEvents"]})
        # re-establish, then reduce by Notification: events off, and a smuggled widening is ignored
        await host.request("featureSets/update", {"effectiveCapabilities": FULL_GRANT})
        await host.expect("channels/register")
        await host.notify("featureSets/update", {"disabled": ["skyrim.events"], "effectiveCapabilities": FULL_GRANT + ["inferenceRequest"]})
        await asyncio.sleep(0.3)
        await game.emit("combat", state="combat", actor={"name": "Bandit"})
        await game.emit("player_chat", text="still here?")
        chat = await host.expect("channels/incoming")
        await asyncio.sleep(0.5)
        no_push = host.none_of("push/event")
        await _teardown(srv, proc)
        return bad, chat, no_push
    bad, chat, no_push = asyncio.run(main())
    assert bad["error"]["code"] == -32602
    assert chat["params"]["messages"][0]["content"][0]["text"] == "still here?"
    assert no_push


def test_chat_falls_back_to_push_without_channels():
    async def main():
        game, srv, proc, host, _, _ = await _setup(["tools", "pushEvents"])
        await asyncio.sleep(0.5)
        await game.emit("player_chat", text="Are you there?")
        push = await host.expect("push/event")
        await _teardown(srv, proc)
        return push
    p = asyncio.run(main())["params"]
    assert {"chat:dm", "chat:from-human", "animunculi:spoken-to"} <= set(p["tags"])
    assert p["payload"]["content"][0]["text"] == "[Skyrim] The player says to you: “Are you there?”"
