// `regions` parallel regions that all react to "flip".
const n = params.regions || 64;
let regions = '';
for (let i = 0; i < n; i++)
  regions += `<state id="r${i}" initial="r${i}off"><state id="r${i}off"><transition event="flip" target="r${i}on"/></state><state id="r${i}on"><transition event="flip" target="r${i}off"/></state></state>`;
return `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="null" name="wide" initial="p">
  <parallel id="p">${regions}</parallel>
</scxml>`;
