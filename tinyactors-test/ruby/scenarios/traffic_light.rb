# frozen_string_literal: true

TinyactorsTest.scenarios 'traffic light', chart: :traffic_light do
  invariant 'exactly one light is on' do |light|
    (light.active & %i[red green yellow flashing]).size == 1
  end

  expect_failure 'host has no definition or actor methods yet', rpc_error: -32601
  scenario 'cycles on logical time' do
    light = spawn
    assert_configuration light, :operating, :red

    advance_time 29_999
    assert_active light, :red
    advance_time 1
    assert_active light, :green
    advance_time 25_000
    assert_active light, :yellow
    advance_time 5_000
    assert_active light, :red
    assert_equal 1, light.data('.cycles')
  end

  # The fault arrives 10s into red and the repair 5s later, so the canceled
  # first timer would be due at t=30s: after red was re-entered at t=15s and
  # 15s before the new timer. A leaked timer turns the light green early.
  expect_failure 'host has no definition or actor methods yet', rpc_error: -32601
  scenario 'repair resumes the interrupted light without stale timers' do
    light = spawn
    advance_time 10_000
    light.deliver 'fault'
    assert_configuration light, :flashing

    advance_time 5_000
    light.deliver 'repair'
    assert_active light, :operating, :red

    advance_time 15_000
    assert_active light, :red
    advance_time 15_000
    assert_active light, :green
    assert_equal 0, light.data('.cycles')
  end
end
