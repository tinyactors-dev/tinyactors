# frozen_string_literal: true

# A parent that delegates to an invoked SCXML child. The child reports
# progress to its parent on entry; the parent autoforwards `finish`, which
# ends the child with done data. The parent logs both, so the child's work
# is observable in the parent's data.
TinyactorsTest.chart :callee do
  data amount: 0

  state :working do
    on_entry { send_event 'progress', to: '#_parent', data: '{amount: .amount}' }
    transition on: 'finish', target: :finished
  end

  final :finished do
    done_data params: { result: '.amount * 2' }
  end
end

TinyactorsTest.chart :caller do
  data log: { expr: '[]' }

  state :calling do
    invoke chart: :callee, id: :worker, params: { amount: '21' }, autoforward: true
    transition on: 'progress', type: :internal do
      assign '.log', '.log + ["progress \($_event.data.amount)"]'
    end
    transition on: 'done.invoke.worker', target: :done do
      assign '.log', '.log + ["done \($_event.data.result)"]'
    end
  end

  final :done
end

# Uses a service that is not an actor (the scenario's invoker.scripted).
TinyactorsTest.chart :service_client do
  data reply: nil

  state :calling do
    invoke type: 'urn:tinyactors:test:service', id: :svc
    transition on: 'reply', target: :replied do
      assign '.reply', '$_event.data'
    end
  end

  state :replied
end
