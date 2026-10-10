'use strict';
// Charts: the helpers write SCXML (one helper per element, identifying
// attributes positionally) through C's XML writer; c.expr functions are
// written as their source and called directly by the native datamodel;
// fromSCXML compiles a document as it is.
const test = require('node:test');
const assert = require('node:assert/strict');
const { scxml, fromSCXML, createSystem, createActor, Chart } = require('./binding');

test('helpers: one per element, positional identifying attributes, then attributes, then children', () => {
  const c = scxml.chart();
  const chart = c.scxml('door', { initial: 'closed', datamodel: 'ecmascript' },
    c.datamodel({ opens: 0, log: [], who: c.expr(() => 42) }),
    c.state('closed',
      c.transition('open', 'opened', { cond: 'opens < 3' }, c.assign('opens', 'opens + 1'), c.log('opened', 'opens'))),
    c.parallel('opened',
      c.state('left', c.onentry(c.raise('ping'), c.send('close', { delay: '5s', id: 'autoClose' }))),
      c.state('right', c.onexit(c.cancel('autoClose')), c.history('h', { type: 'deep' }))),
    c.final('gone', c.donedata(c.param('n', 'opens'), c.content('x'))),
    c.state('scripted', c.onentry(c.script('opens = 0'), c.if('opens', c.raise('a'), c.elseif('false'), c.else(), c.raise('b')),
      c.foreach('log', 'item', { index: 'i' }, c.log(undefined, 'item')))),
  );
  assert.ok(chart instanceof Chart);
  assert.equal(chart.name, 'door');
  const xml = chart.xml;
  assert.match(xml, /^<scxml xmlns="http:\/\/www.w3.org\/2005\/07\/scxml" version="1.0" name="door" initial="closed" datamodel="ecmascript">/);
  for (const piece of [
    '<data id="opens" expr="0"/>',
    '<data id="log" expr="[]"/>',
    '<data id="who" expr="() => 42"/>',
    '<transition event="open" target="opened" cond="opens &lt; 3">',
    '<assign location="opens" expr="opens + 1"/>',
    '<log label="opened" expr="opens"/>',
    '<parallel id="opened">',
    '<raise event="ping"/>',
    '<send event="close" delay="5s" id="autoClose"/>',
    '<cancel sendid="autoClose"/>',
    '<history id="h" type="deep"/>',
    '<param name="n" expr="opens"/>',
    '<content>x</content>',
    '<script>opens = 0</script>',
    '<if cond="opens">',
    '<elseif cond="false"/>',
    '<else/>',
    '<foreach array="log" item="item" index="i">',
    '<log expr="item"/>',
  ])
    assert.ok(xml.includes(piece), `${piece} in\n${xml}`);
});

test('c.expr: written as the source, unique per function, called with the scope (closures work)', () => {
  const c = scxml.chart();
  const step = 7;
  const add = (n) => c.expr(({ count }) => count + n);
  const chart = c.scxml('counter', { initial: 'on', datamodel: 'ecmascript' },
    c.datamodel({ count: 0 }),
    c.state('on',
      c.transition('add1', undefined, c.assign('count', add(1))),
      c.transition('add2', undefined, c.assign('count', add(2))),
      c.transition('step', undefined, c.assign('count', c.expr(({ count }) => count + step))),
      c.transition('who', undefined, c.assign('count', c.expr(({ _event, _sessionid, In }) => `${_event.name} ${typeof _sessionid} ${In('on')}`)))));
  assert.ok(chart.xml.includes('expr="({ count }) => count + n"'));
  assert.ok(chart.xml.includes('expr="({ count }) => count + n /* 2 */"'), 'the same source twice: told apart');
  const system = createSystem();
  const a = createActor(chart, { system }).start();
  a.send({ type: 'add1' });
  a.send({ type: 'add2' });
  a.send({ type: 'step' });
  assert.equal(a.getSnapshot().context.count, 10);
  a.send({ type: 'who' });
  assert.equal(a.getSnapshot().context.count, 'who string true');
  system.stop();
});

test('fromSCXML: a document as it is; documents answer src; errors name the line', () => {
  const doc = fromSCXML(`<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="parent" initial="a">
    <state id="a"><invoke src="child.scxml" id="kid"/><transition event="done.invoke.kid" target="z"/></state>
    <final id="z"/></scxml>`, {
    documents: { 'child.scxml': '<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" initial="c"><final id="c"/></scxml>' },
  });
  const system = createSystem();
  const a = createActor(doc, { system }).start();
  assert.equal(a.getSnapshot().status, 'done');
  assert.equal(a.getSnapshot().value, 'z');
  assert.throws(() => system.load([fromSCXML('<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" initial="nowhere"><state id="a"/></scxml>')]), /SCXML compilation failed:\n {2}chart~2:1:1: unknown state id .nowhere./);
  system.stop();
});

test('versions: loading a chart name again makes new actors run the new version', () => {
  const v = (n) => fromSCXML(`<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="greeter" initial="s">
    <datamodel><data id="version" expr="${n}"/></datamodel><state id="s"/></scxml>`);
  const v1 = v(1);
  const system = createSystem();
  const a = createActor(v1, { system }).start();
  system.load([v(2)]);
  const b = createActor(v1, { system }).start(); /* by name: the current version */
  assert.equal(a.getSnapshot().context.version, 1);
  assert.equal(b.getSnapshot().context.version, 2);
  system.stop();
});
