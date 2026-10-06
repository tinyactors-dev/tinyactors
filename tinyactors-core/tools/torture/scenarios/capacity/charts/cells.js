// cells: a parent invokes `fanout` children; every second it ticks all of
// them and each child replies — static #_parent / #_<id> routing, null datamodel.
const n = params.fanout || 8;
const NS = 'xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="null"';
let invokes = '', ticks = '';
for (let i = 0; i < n; i++) {
  invokes += `
    <invoke id="c${i}">
      <content>
        <scxml ${NS} initial="w">
          <state id="w"><transition event="tick"><send event="ack" target="#_parent"/></transition></state>
        </scxml>
      </content>
    </invoke>`;
  ticks += `<send event="tick" target="#_c${i}"/>`;
}
return `<scxml ${NS} name="cell" initial="run">
  <state id="run">
    <onentry><send event="beat" delay="1s" id="beat"/></onentry>${invokes}
    <transition event="beat">${ticks}<send event="beat" delay="1s" id="beat"/></transition>
    <transition event="ack"/>
  </state>
</scxml>`;
