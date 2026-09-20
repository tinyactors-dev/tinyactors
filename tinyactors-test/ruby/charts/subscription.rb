# frozen_string_literal: true

# Two revisions of the definition family "subscription": the chart that
# reacts to the events of a subscription that lives outside the actor
# system (a payment provider's). The actor holds no subscription state of
# its own beyond what its host binding carries — the provider is the record
# of truth — so upgrading the chart is a definition upgrade of a long-lived
# named actor (docs/manual/persistence.md, "Definition upgrades").
#
# v1 suspends the subscription as soon as a payment fails.
TinyactorsTest.chart :subscription_v1, document_name: 'subscription' do
  data account: 'acct_1'
  data failures: 0

  initial :active

  state :active do
    transition on: 'payment.failed', target: :past_due do
      assign '.failures', '.failures + 1'
    end
  end

  state :past_due do
    transition on: 'payment.succeeded', target: :active
  end
end

# v2 is the upgrade: a failed payment first enters a dunning grace period
# and only falls to past_due when the grace period expires. It keeps the
# data v1 collected, because the host binds the replacement to it.
TinyactorsTest.chart :subscription_v2, document_name: 'subscription' do
  data account: 'acct_1'
  data failures: 0

  initial :active

  state :active do
    transition on: 'payment.failed', target: :dunning do
      assign '.failures', '.failures + 1'
    end
  end

  state :dunning do
    on_entry { send_event 'dunning.expired', delay: '3s', id: :dunning }
    on_exit { cancel :dunning }
    transition on: 'payment.succeeded', target: :active
    transition on: 'dunning.expired', target: :past_due
  end

  state :past_due do
    transition on: 'payment.succeeded', target: :active
  end
end
