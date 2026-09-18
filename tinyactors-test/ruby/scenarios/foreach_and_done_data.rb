# frozen_string_literal: true

# <foreach>, <donedata>, and actor completion, with expression data only
# (literal content and $_event belong to other features).

# Sums an array, iterates a snapshot while the array grows, and reports
# errors from an illegal collection and an illegal item name.
TinyactorsTest.chart :foreach_sum do
  data numbers: { expr: '[1, 2, 3]' }
  data total: 0
  data visits: 0
  data last_index: nil
  data errors: 0

  state :summing do
    on_entry do
      foreach '.numbers', item: 'n', index: 'i' do
        assign '.total', '.total + .n'
        assign '.numbers', '.numbers + [10]'
        assign '.visits', '.visits + 1'
        assign '.last_index', '.i'
      end
      foreach '7', item: 'n' do
        assign '.visits', '.visits + 100'
      end
      assign '.visits', '.visits + 1000'
    end
    transition on: 'error.execution', target: :bad_item do
      assign '.errors', '.errors + 1'
    end
  end

  state :bad_item do
    on_entry do
      foreach '[1]', item: "'continue'" do
        assign '.visits', '.visits + 100'
      end
    end
    transition on: 'error.execution', target: :finished do
      assign '.errors', '.errors + 1'
    end
  end

  final :finished
end

# done.state events follow a final child; a top-level final state's
# <donedata> becomes the actor's completion.
TinyactorsTest.chart :done_data_completion do
  data seen: nil

  state :work, initial: :step do
    transition on: 'done.state.work', target: :result do
      assign '.seen', '"done.state.work"'
    end
    state :step do
      transition target: :stepped
    end
    final :stepped do
      done_data params: { inner: 'true' }
    end
  end

  final :result do
    done_data expr: '{answer: 42}'
  end
end

TinyactorsTest.chart :done_data_error do
  state :work do
    transition target: :result
  end
  final :result do
    done_data expr: 'error("boom")'
  end
end

TinyactorsTest.scenarios 'foreach and done data' do
  scenario 'foreach iterates a snapshot and binds item and index' do
    actor = spawn :foreach_sum
    assert actor.done?, 'the actor finishes'
    assert_equal 6, actor.data('.total')
    assert_equal 3, actor.data('.visits'), 'an illegal collection stops the enclosing block'
    assert_equal 2, actor.data('.last_index')
    assert_equal 2, actor.data('.errors')
  end

  scenario 'a top-level final state reports its donedata as the completion' do
    actor = spawn :done_data_completion
    assert actor.done?, 'the actor finishes'
    assert_equal 'done.state.work', actor.data('.seen')
    assert_equal({ answer: 42 }, actor.completion)
  end

  scenario 'a failing donedata leaves the completion empty' do
    actor = spawn :done_data_error
    assert actor.done?, 'the actor finishes'
    assert_equal nil, actor.completion
  end
end
