// The MCP suite's fixture program for @tinyactors/mcp
// (tinyactors-test/docs/mcp.md): MCP's stdio transport on standard input
// and output, with the fixture's methods (fixture/open, settle, advance,
// spawn) answered here and every other message given to the server.
// TA_NAPI_DATAMODEL picks the host's datamodel (native by default).
import { createInterface } from 'node:readline';
import { compileCharts, Host, SimulatedClock } from '@tinyactors/node';
import { createMCP, tap } from '../index.mjs';

const write = (message) => process.stdout.write(`${JSON.stringify(message)}\n`);

// The transport the server is connected to: messages from the runner that
// are not the fixture's, and the server's out.
const transport = {
  async start() {},
  async send(message) { write(message); },
  async close() { this.onclose?.(); },
};

let fixture;

// A service of the fixture: it answers as the fixture says (docs/mcp.md, "Fixtures").
const service = (answer) => (invocation) => {
  if (answer !== 'silent') {
    queueMicrotask(() => {
      if (answer === 'done') invocation.done();
      else invocation.sendBack(answer === 'error' ? 'error.communication' : answer);
    });
  }
  return {};
};

const methods = {
  async open(params) {
    const clock = new SimulatedClock();
    const compiled = compileCharts(params.charts);
    const t = tap();
    const host = new Host(compiled.image, t.options({
      system: params.system,
      config: params.config,
      datamodel: process.env.TA_NAPI_DATAMODEL ?? 'native',
      clock,
      services: Object.fromEntries(Object.entries(params.services ?? {}).map(([type, answer]) => [type, service(answer)])),
    }));
    compiled.close();
    const mcp = createMCP({ [params.system]: { host, tap: t, sources: { ...params.sources } } }, params.options ?? {});
    await mcp.server().connect(transport);
    fixture = { host, clock };
    await host.idle();
    return {};
  },
  async settle() {
    await fixture.host.idle();
    return {};
  },
  async advance({ ms }) {
    fixture.clock.increment(ms);
    await fixture.host.idle();
    return {};
  },
  async spawn({ chart, name, params }) {
    const actor = fixture.host.spawn(chart, { name, params });
    return { session: Number(actor.id) };
  },
};

const lines = createInterface({ input: process.stdin });
let queue = Promise.resolve();
lines.on('line', (line) => {
  if (!line.trim()) return;
  const message = JSON.parse(line);
  if (typeof message.method === 'string' && message.method.startsWith('fixture/')) {
    // in order, each after the last: a fixture step waits for what came before it
    queue = queue.then(async () => {
      try {
        const result = await methods[message.method.slice('fixture/'.length)](message.params ?? {});
        write({ jsonrpc: '2.0', id: message.id, result });
      } catch (error) {
        write({ jsonrpc: '2.0', id: message.id, error: { code: -32000, message: String(error?.stack ?? error) } });
      }
    });
  } else {
    queue = queue.then(() => transport.onmessage?.(message));
  }
});
lines.on('close', async () => {
  await queue;
  fixture?.host.close();
  process.exit(0);
});
