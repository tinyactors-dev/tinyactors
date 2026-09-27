# frozen_string_literal: true

# The `spinner` chart of the shared DAP transcripts (../README.md): `spin`
# starts a macrostep that never ends. spinner.js is the same chart for napi.
TinyactorsTest.chart :spinner do
  data n: 0
  initial :resting

  state :resting do
    transition on: 'spin', target: :spinning
  end

  state :spinning do
    transition cond: 'true', target: :spinning do
      assign '.n', '.n + 1'
    end
  end
end
