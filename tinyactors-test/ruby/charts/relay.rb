# frozen_string_literal: true

# One external event (`go`) starts a macrostep of many microsteps: the `go`
# transition sends `ping` to itself through the router and raises the
# internal event `counted`; an eventless loop then counts `.n` to 5, one
# microstep per increment, before `counted` is processed. Every handled
# event is appended to `.log`, so processing order is observable:
# go, n=5, counted, then the external events (ping, extra) in arrival order.
TinyactorsTest.chart :relay do
  data log: { expr: '[]' }
  data n: 0

  state :relay, initial: :idle do
    transition on: 'ping', type: :internal do
      assign '.log', '.log + ["ping"]'
    end
    transition on: 'extra', type: :internal do
      assign '.log', '.log + ["extra"]'
    end

    state :idle do
      transition on: 'go', target: :counting do
        assign '.log', '.log + ["go"]'
        send_event 'ping'
        raise_event 'counted'
      end
    end

    state :counting do
      transition cond: '.n < 5', type: :internal do
        assign '.n', '.n + 1'
      end
      transition cond: '.n >= 5', target: :reported do
        assign '.log', '.log + ["n=\(.n)"]'
      end
    end

    state :reported do
      transition on: 'counted', target: :idle do
        assign '.log', '.log + ["counted"]'
      end
    end
  end
end
