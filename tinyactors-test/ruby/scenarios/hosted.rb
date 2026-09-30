# frozen_string_literal: true

# Hosted mode (kernel ADR 0001 section 5): systems run by the host kernel
# (tinyactors-host) instead of by core calls a scenario makes one by one.
# The kernel queues, schedules and routes; the scenario runs turns and owns
# the clock.
TinyactorsTest.scenarios 'hosted' do
  # Catches: the test host not linking the kernel, or a hosted system that
  # leaks what the kernel allocated for it.
  scenario 'a hosted system opens and closes without leaking' do
    system = hosted_system
    assert_equal 0, close_hosted(system), 'the kernel freed everything'
  end
end
