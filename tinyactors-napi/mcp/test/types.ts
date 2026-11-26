// index.d.ts against the README's use; checked with tsc (mise run lint).
import { createMCP, tap } from '@tinyactors/mcp';
import { Host } from '@tinyactors/node';

declare const image: Uint8Array;
const t = tap({ spans: 1000 });
const host = new Host(image, t.options({ system: 'shop' }));
const mcp = createMCP({ shop: { host, tap: t, sources: { order: '<scxml/>' } } }, { allowDeploy: true });
const served = await mcp.listen({ port: 7333, token: 'secret' });
const url: string = served.url;
await served.close();
mcp.server();
void url;
