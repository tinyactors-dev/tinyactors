// The spike's ant chart: the states of a worker's foraging loop, plus the
// egg and larva stages, each a delayed send to itself. The world (the I/O
// processor) owns geometry and time: it answers each decision with an
// "arrive" (or "found"/"fight") at the simulated time the leg ends.
export const antChart = `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="ant" initial="start">
  <datamodel>
    <data id="colony" expr="0"/>
    <data id="stage" expr="'worker'"/>
    <data id="hp" expr="2"/>
  </datamodel>
  <state id="start">
    <transition cond="stage === 'egg'" target="egg"/>
    <transition cond="stage === 'idle'" target="idle"/>
    <transition target="worker"/>
  </state>
  <state id="idle"/>
  <state id="egg">
    <onentry><send event="hatch" delay="20s"/></onentry>
    <transition event="hatch" target="larva"/>
  </state>
  <state id="larva">
    <onentry><send event="grow" delay="30s"/></onentry>
    <transition event="grow" target="worker"/>
  </state>
  <state id="worker" initial="out">
    <transition event="kill" target="dead"/>
    <transition event="fight" cond="hp &lt;= 1" target="dead"/>
    <transition event="fight"><assign location="hp" expr="hp - 1"/><send type="urn:world" event="flee"/></transition>
    <state id="out">
      <onentry><send type="urn:world" event="forage"/></onentry>
      <transition event="found" target="carrying"/>
      <transition event="arrive" target="out"/>
    </state>
    <state id="carrying">
      <onentry><send type="urn:world" event="home"/></onentry>
      <transition event="arrive" target="rest"/>
    </state>
    <state id="rest">
      <onentry><send type="urn:world" event="rest"/></onentry>
      <transition event="arrive" target="out"/>
    </state>
  </state>
  <final id="dead"><onentry><send type="urn:world" event="died"/></onentry></final>
</scxml>`;
