# frozen_string_literal: true

# Edit this file while the workbench runs: on save, every actor of this
# chart's family is replaced in place, keeping the data it has accumulated.
TinyactorsTest.chart :subscription do
  data account: 'acct_1'
  data failures: 0

  initial :active

  state :active do
    on_entry { log 'entered', expr: '"active"' }
    transition on: 'payment.failed', target: :dunning do
      assign '.failures', '.failures + 1'
    end
  end

  state :dunning do
    on_entry { log 'entered', expr: '"dunning"' }
    on_entry { send_event 'dunning.expired', delay: '3s', id: :dunning }
    on_exit { cancel :dunning }
    transition on: 'payment.succeeded', target: :active
    transition on: 'dunning.expired', target: :past_due
  end

  state :past_due do
    on_entry { log 'entered', expr: '"past_due"' }
    transition on: 'payment.succeeded', target: :active
  end
end
