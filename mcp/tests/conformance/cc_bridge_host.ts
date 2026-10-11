#!/usr/bin/env bun
/**
 * Conformance run against a real MCPL host implementation: the Claude Code bridge's
 * McplServerHandle (anima-research/mcpl-cc-bridge) hosts `animunculi-mcpl` over stdio, with the
 * Python mock game standing in for Skyrim.
 *
 *   MCPL_BRIDGE_DIR=<mcpl-cc-bridge checkout>/plugins/mcpl-bridge bun run tests/conformance/cc_bridge_host.ts
 *
 * Checks: the bridge accepts our manifest and grants what we advertise (and no `observe`), the
 * feature sets derive fully, the chat channel registers, a typed player line arrives as a
 * chat:dm channel message, a fight as a tagged push event, the presence hook injects a
 * beforeUser line, publishing speaks in game, and opening the channel lets ambient speech in.
 */
import { spawn } from 'child_process'
import { connect } from 'net'
import { join, resolve } from 'path'

const BRIDGE = process.env.MCPL_BRIDGE_DIR ?? ''
if (!BRIDGE) {
  console.error('set MCPL_BRIDGE_DIR to <mcpl-cc-bridge checkout>/plugins/mcpl-bridge')
  process.exit(2)
}
const MCP_DIR = resolve(import.meta.dir, '..', '..')
const PYTHON = process.env.PYTHON ?? 'python'
const PORT = 47000 + Math.floor(Math.random() * 900)
const TOKEN = 'conformance'

const { McplServerHandle } = await import(join(BRIDGE, 'src', 'mcpl-host.ts'))

let failures = 0
const check = (cond: boolean, label: string, detail?: unknown) => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${!cond && detail !== undefined ? `  -> ${JSON.stringify(detail).slice(0, 400)}` : ''}`)
  if (!cond) failures++
}
const sleep = (ms: number) => new Promise(r => setTimeout(r, ms))

// ---- the mock game --------------------------------------------------------------------------
const env = { ...process.env, PYTHONPATH: join(MCP_DIR, 'src') }
const game = spawn(PYTHON, ['-u', '-m', 'animunculi_mcp.mock_game', '--port', String(PORT), '--token', TOKEN, '--chatter', '0'],
  { env, stdio: ['ignore', 'pipe', 'inherit'] })
await new Promise<void>(res => game.stdout!.once('data', () => res()))

function gameCall(method: string, params: Record<string, unknown>): Promise<unknown> {
  return new Promise((res, rej) => {
    const sock = connect(PORT, '127.0.0.1')
    let buf = ''
    sock.on('data', d => {
      buf += d.toString()
      const lines = buf.split('\n')
      buf = lines.pop() ?? ''
      for (const line of lines) {
        const msg = JSON.parse(line)
        if (msg.id === 1) sock.write(JSON.stringify({ id: 2, method, params }) + '\n')
        if (msg.id === 2) { sock.end(); msg.error ? rej(msg.error) : res(msg.result) }
      }
    })
    sock.on('error', rej)
    sock.write(JSON.stringify({ id: 1, method: 'hello', params: { token: TOKEN } }) + '\n')
  })
}
const emit = (type: string, data: Record<string, unknown>) => gameCall('debug_emit', { type, data })

// ---- the host ---------------------------------------------------------------------------------
const deliveries: Array<{ kind: string; text: string; meta: Record<string, string> }> = []
const logs: string[] = []
const handle = new McplServerHandle('animunculi', {
  transport: {
    command: PYTHON,
    args: ['-m', 'animunculi_mcp.mcpl.server'],
    env: { ...env, ANIMUNCULI_HOST: '127.0.0.1', ANIMUNCULI_PORT: String(PORT), ANIMUNCULI_TOKEN: TOKEN },
  },
  // the bridge's documented default grant, spelled out
  grant: ['tools', 'pushEvents', 'modelInfo', 'inferenceRequest', 'inferenceLifecycle', 'channels.register',
          'channels.lifecycle', 'channels.incoming', 'channels.publish', 'channels.acknowledge',
          'contextHooks.beforeInference.observe', 'contextHooks.beforeInference.inject.beforeUser'],
  enableFeatureSets: ['*'],
}, {
  deliver: (d: { kind: string; text: string; meta: Record<string, string> }) => deliveries.push(d),
  toolsChanged: () => {},
  log: (line: string) => { logs.push(line); if (process.env.VERBOSE) console.log(`  | ${line}`) },
})

async function waitFor<T>(fn: () => T | undefined, ms = 8000): Promise<T | undefined> {
  const end = Date.now() + ms
  while (Date.now() < end) {
    const v = fn()
    if (v) return v
    await sleep(100)
  }
  return undefined
}

try {
  handle.start()
  await handle.firstAttempt
  await waitFor(() => (handle.status === 'ready' ? true : undefined), 10000)
  check(handle.status === 'ready', 'bridge completes the MCPL handshake', { status: handle.status, logs: logs.slice(-5) })
  check(!handle.grant.includes('contextHooks.beforeInference.observe'), 'observe is not granted (we never advertise it)', handle.grant)
  check(handle.grant.includes('contextHooks.beforeInference.inject.beforeUser') && handle.grant.includes('pushEvents'),
    'grant = advertised ∩ policy', handle.grant)
  const allSets = ['skyrim.events', 'skyrim.chat', 'skyrim.chat.overhear', 'skyrim.presence', 'skyrim.thinking']
  check(allSets.every(s => handle.featureSetsEnabled.includes(s)), 'every feature set derives as enabled', handle.featureSetsEnabled)
  check(handle.tools.length >= 30 && handle.tools.some((t: { name: string }) => t.name === 'observe'), 'tools proxied', handle.tools.length)

  const registered = await waitFor(() => (handle.channels.has('skyrim:default') ? true : undefined))
  check(!!registered, 'channel skyrim:default registered')

  await emit('player_chat', { text: 'Ready when you are.', source: 'chat_window' })
  const chat = await waitFor(() => deliveries.find(d => d.kind === 'channel-message' && d.text.includes('Ready when you are')))
  check(!!chat && chat.meta.tags?.includes('chat:dm') && chat.meta.tags?.includes('chat:addressed'),
    'typed player line -> channel message tagged chat:dm (closure adds chat:addressed)', chat)

  await emit('combat', { state: 'combat', actor: { name: 'Draugr' }, target: { name: 'Lydia', agent: 'default' } })
  const push = await waitFor(() => deliveries.find(d => d.kind === 'push-event' && d.text.includes('Draugr')))
  check(!!push && push.meta.tags?.includes('animunculi:combat') && push.meta.feature_set === 'skyrim.events',
    'fight -> push/event with producer tags', push)

  const inj = await handle.beforeInference('what is the plan?', 'inf-1', 'conv', 0)
  check(inj.length === 1 && inj[0].position === 'beforeUser' && String(inj[0].content).startsWith('[Skyrim'),
    'presence hook injects one beforeUser line', inj)

  const pub = await handle.publish('skyrim:default', 'Lead on, I will follow.')
  check((pub as { delivered?: boolean })?.delivered === true, 'publish -> spoken in game', pub)

  await handle.openChannel('skyrim:default')
  await emit('heard', { speaker: { id: '0x2', name: 'Tolfdir' }, text: 'Careful with that!', dist_m: 4 })
  const ambient = await waitFor(() => deliveries.find(d => d.kind === 'channel-message' && d.text.includes('Careful with that')))
  check(!!ambient && ambient.meta.tags?.includes('chat:ambient'), 'open channel -> overheard speech as chat:ambient', ambient)

  handle.lifecycle({ inferenceId: 'inf-1', conversationId: 'conv', turnIndex: 0, phase: 'started' })
  const res = (await handle.callTool('observe', {})) as { content?: Array<{ text: string }> }
  check(!!res?.content?.[0]?.text?.includes('Lydia'), 'tools/call through the host', res)
} catch (e) {
  check(false, 'run', String(e))
} finally {
  handle.close()
  game.kill()
}
console.log(failures ? `${failures} FAILED` : 'all passed')
process.exit(failures ? 1 : 0)
