'use strict';

// The `spinner` chart of the shared DAP transcripts (../README.md): `spin`
// starts a macrostep that never ends. spinner.rb is the same chart for the
// test host. The runner passes napi's statechart().
module.exports = (statechart) => statechart({ family: 'spinner', revision: 'v1' })
  .data('n', 0)
  .state('resting', (s) => s
    .on('spin', (t) => t.target('spinning')))
  .state('spinning', (s) => s
    .always((t) => t.when(() => true).target('spinning')
      .assign('n', function () { return this.n + 1; })));
