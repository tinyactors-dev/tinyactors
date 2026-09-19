# frozen_string_literal: true

# Cancels a delayed send with the ID `light`, which it never sent itself;
# the traffic light's timers use that ID.
TinyactorsTest.chart :canceller do
  state :waiting do
    transition on: 'cancel_it', type: :internal do
      cancel 'light', unmatched: true
    end
  end
end

# `arm` schedules `fire` in 10s with data {v: .v}, then changes .v in the
# same transition; `bump` changes it again before the send is due. `fire`
# stores the value it carried in .got.
TinyactorsTest.chart :freezer do
  data v: 1
  data got: nil

  state :ready do
    transition on: 'arm', type: :internal do
      send_event 'fire', delay: '10s', id: :shot, data: '{v: .v}'
      assign '.v', '2'
    end
    transition on: 'bump', type: :internal do
      assign '.v', '.v + 10'
    end
    transition on: 'fire', type: :internal do
      assign '.got', '$_event.data.v'
    end
  end
end
