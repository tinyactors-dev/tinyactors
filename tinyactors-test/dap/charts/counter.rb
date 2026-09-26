# frozen_string_literal: true

# The `counter` chart of the shared DAP transcripts (../README.md), in the
# test host's authoring form. counter.js is the same chart for napi: keep
# the two in step, with the same state IDs, events, data and actions in the
# same order. Data is declared in alphabetical order, the order the jq
# data model lists it in.
TinyactorsTest.chart :counter do
  data count: 0
  data it: nil
  data items: [1, 2]
  data last: nil
  data ticks: 0
  initial :idle

  state :idle do
    transition on: 'bump', target: :busy do
      assign '.count', '.count + 1'
      assign '.last', '$_event.data'
    end
    transition on: 'ping', type: :internal do
      send_event 'pong', to_expr: '$_event.origin', type_expr: '$_event.origintype', data: '.count'
    end
    transition on: 'fail', type: :internal do
      assign '.count', 'error("boom")'
    end
    transition on: 'error.execution', type: :internal do
      assign '.last', '"failed"'
    end
    transition on: 'remind', type: :internal do
      send_event 'tick', delay: '60s', id: :nudge
    end
    transition on: 'tick', type: :internal do
      assign '.ticks', '.ticks + 1'
    end
  end

  state :busy do
    on_entry do
      log 'busy', expr: '.count'
      foreach '.items', item: 'it' do
        assign '.count', '.count + .it'
      end
    end
    on_exit do
      log 'leaving', expr: '.count'
    end
    transition on: 'done', target: :idle
  end
end
