import { compileCharts, Host, SimulatedClock } from '@tinyactors/wasm';
const c = compileCharts([{ name: 'a.scxml', text: `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="a" initial="s">
<datamodel><data id="k" expr="0"/></datamodel>
<state id="s"><onentry><send type="urn:w" event="hi"><param name="k" expr="k"/></send></onentry>
<transition event="die" target="d"/></state><final id="d"/></scxml>` }]);
const clock = new SimulatedClock();
const host = new Host(c, { system: 'p', clock, ioProcessors: { 'urn:w': { receive(s) { console.log('recv', s.session, s.event, s.data, s.target); } } }, onDone: (a) => console.log('done', a.id) });
const a = [];
for (let i = 0; i < 3; i++) a.push(host.spawn('a', { params: { k: i * 10 } }));
await host.idle();
console.log(a.map((x) => x.id));
host.send(a[1], 'die'); await host.idle();
const b = host.spawn('a'); await host.idle(); console.log('reused', b.id);
console.log(Object.keys(host), Object.getOwnPropertyNames(Object.getPrototypeOf(Object.getPrototypeOf(host))).join(' '));
host.close();
