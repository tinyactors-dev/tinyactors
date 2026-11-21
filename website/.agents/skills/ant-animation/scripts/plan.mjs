// Prints both ants' positions and headings every 0.8 s: tune a scene's cues
// against this before rendering (node plan.mjs).
import { makeScene } from './scene.mjs';
const s = makeScene();
for (let t = 0; t <= 31; t += 0.8) { s.advanceTo(t); const f = (a) => `${a.x.toFixed(0)},${a.y.toFixed(0)} h${(a.heading%360).toFixed(0)}`; console.log(t.toFixed(1), 'A', f(s.ants.a), ' B', s.ants.b.hidden ? '-' : f(s.ants.b)); }
