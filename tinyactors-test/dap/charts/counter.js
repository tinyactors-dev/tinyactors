'use strict';

// The `counter` chart of the shared DAP transcripts (../README.md), in
// napi's authoring form. counter.rb is the same chart for the test host:
// keep the two in step, with the same state IDs, events, data and actions
// in the same order. Data is declared in alphabetical order, the order the
// test host's jq data model lists it in. The runner passes napi's statechart().
module.exports = (statechart) => statechart({ family: 'counter', revision: 'v1' })
  .data('count', 0)
  .data('it', null)
  .data('items', [1, 2])
  .data('last', null)
  .data('ticks', 0)
  .state('idle', (s) => s
    .on('bump', (t) => t.target('busy')
      .assign('count', function () { return this.count + 1; })
      .assign('last', ({ event }) => event.data ?? null))
    .on('ping', (t) => t.internal().send('pong', (send) => send
      .via(({ event }) => event.originType)
      .to(({ event }) => event.origin)
      .data(function () { return this.count; })))
    .on('fail', (t) => t.internal().assign('count', () => { throw new Error('boom'); }))
    .on('error.execution', (t) => t.internal().assign('last', 'failed'))
    .on('remind', (t) => t.internal().send('tick', (send) => send.id('nudge').after('60s')))
    .on('tick', (t) => t.internal().assign('ticks', function () { return this.ticks + 1; })))
  .state('busy', (s) => s
    .entry((e) => e
      .log('busy', function () { return this.count; })
      .forEach(function () { return this.items; }, 'it', (body) => body
        .assign('count', function () { return this.count + this.it; })))
    .exit((e) => e
      .log('leaving', function () { return this.count; }))
    .on('done', (t) => t.target('idle')));
