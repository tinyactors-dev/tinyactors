# frozen_string_literal: true

# A traffic light driven entirely by delayed sends on logical time. A fault
# switches to flashing; a repair resumes the interrupted light through shallow
# history. Every light cancels its timer on exit so a timer from an earlier
# visit cannot fire after the light is re-entered.
TinyactorsTest.chart :traffic_light do
  data cycles: 0

  state :operating, initial: :red do
    transition on: 'fault', target: :flashing
    history :resume, type: :shallow, default: :red

    state :red do
      on_entry { send_event 'timer', delay: '30s', id: :light }
      on_exit { cancel :light }
      transition on: 'timer', target: :green
    end

    state :green do
      on_entry { send_event 'timer', delay: '25s', id: :light }
      on_exit { cancel :light }
      transition on: 'timer', target: :yellow
    end

    state :yellow do
      on_entry { send_event 'timer', delay: '5s', id: :light }
      on_exit { cancel :light }
      transition on: 'timer', target: :red do
        assign '.cycles', '.cycles + 1'
      end
    end
  end

  state :flashing do
    transition on: 'repair', target: :resume
  end
end
