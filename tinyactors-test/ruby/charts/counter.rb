# frozen_string_literal: true

# Two revisions of the definition family "counter", for actor replacement
# (ADR 0037). v1 adds 1 per `tick`. v2 adds 10 per `tick`, keeps the
# count it is bound to, and announces itself on entry by sending `hello`
# to itself: a bootstrap effect that must stay held until activation.
TinyactorsTest.chart :counter_v1, document_name: 'counter' do
  data count: 0

  state :counting do
    transition on: 'tick', type: :internal do
      assign '.count', '.count + 1'
    end
  end
end

TinyactorsTest.chart :counter_v2, document_name: 'counter' do
  data count: 0
  data greeted: false

  state :counting do
    on_entry { send_event 'hello' }
    transition on: 'tick', type: :internal do
      assign '.count', '.count + 10'
    end
    transition on: 'hello', type: :internal do
      assign '.greeted', 'true'
    end
  end
end

# Invokes two services that are not actors (invoker.scripted) and logs
# every error event with its type and envelope invocation ID, in order.
TinyactorsTest.chart :service_pair do
  data errors: { expr: '[]' }
  data replies: { expr: '[]' }

  state :calling do
    invoke type: 'urn:tinyactors:test:service', id: :first
    invoke type: 'urn:tinyactors:test:service', id: :second
    transition on: 'error', type: :internal do
      assign '.errors', '.errors + [{name: $_event.name, type: $_event.type, invokeid: $_event.invokeid}]'
    end
    transition on: 'reply', type: :internal do
      assign '.replies', '.replies + [$_event.invokeid]'
    end
  end
end
