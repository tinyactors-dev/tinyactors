// Generates the storm chart: a root that invokes `children` children which
// ping-pong with it forever. Null datamodel: routing is purely static.
const n = params.children || 16;
const NS = 'xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="null"';
let invokes = '';
let replies = '';
for (let i = 0; i < n; i++) {
  invokes += `
    <invoke id="c${i}">
      <content>
        <scxml ${NS} initial="s">
          <state id="s">
            <onentry><send event="ping${i}" target="#_parent"/></onentry>
            <transition event="pong" target="s"/>
          </state>
        </scxml>
      </content>
    </invoke>`;
  replies += `
    <transition event="ping${i}"><send event="pong" target="#_c${i}"/></transition>`;
}
return `<scxml ${NS} name="storm" initial="run">
  <state id="run">${invokes}${replies}
  </state>
</scxml>`;
