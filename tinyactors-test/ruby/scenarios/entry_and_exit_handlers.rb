# frozen_string_literal: true

# Several <onentry> and <onexit> handlers on one state (SCXML 3.8, 3.9). Each
# handler is a separate block of executable content: an error skips the rest
# of its own handler, and the handlers after it still run, in document order.

TinyactorsTest.chart :entry_and_exit_handlers do
  data trace: { expr: '[]' }

  state :working do
    on_entry do
      assign '.trace', '.trace + ["entry 1"]'
      assign '.trace', 'error("boom")'
      assign '.trace', '.trace + ["entry 1, skipped"]'
    end
    on_entry do
      assign '.trace', '.trace + ["entry 2"]'
    end
    on_exit do
      assign '.trace', '.trace + ["exit 1"]'
      assign '.trace', 'error("boom")'
      assign '.trace', '.trace + ["exit 1, skipped"]'
    end
    on_exit do
      assign '.trace', '.trace + ["exit 2"]'
    end
    transition on: 'finish', target: :finished
  end

  final :finished
end

TinyactorsTest.scenarios 'entry and exit handlers' do
  scenario 'an error in one handler does not skip the next handler' do
    actor = spawn :entry_and_exit_handlers
    assert_active actor, :working
    assert_equal ['entry 1', 'entry 2'], actor.data('.trace')

    actor.deliver 'finish'
    assert actor.done?, 'the actor finishes'
    assert_equal ['entry 1', 'entry 2', 'exit 1', 'exit 2'], actor.data('.trace')
  end
end
