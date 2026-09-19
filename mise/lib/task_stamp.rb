# frozen_string_literal: true

require 'json'
require 'pathname'

# mise decides whether to skip a task from its `sources` alone (checked with
# mise 2026.6.14). A task whose declared `outputs` were deleted is skipped
# although its outputs are gone, so `rm -rf */build` followed by
# `mise run test` fails on missing binaries instead of rebuilding them.
#
# Task files pass TaskStamp.force(root, task) to `mise run` so that a task
# with missing outputs runs anyway. Outputs are paths relative to the
# project root; a task whose outputs mise computes itself
# (`outputs={auto=true}`) has none to check here.
module TaskStamp
  module_function

  # The `#MISE outputs=[...]` of a task file, as paths under `root`.
  def outputs(root, task)
    file = Pathname.new(root).join('mise/tasks', task.to_s)
    return [] unless file.file?

    line = file.each_line.find { |text| text.start_with?('#MISE outputs=') }
    return [] unless line

    JSON.parse(line.delete_prefix('#MISE outputs=')).map { |path| Pathname.new(root).join(path) }
  rescue JSON::ParserError
    # outputs={auto=true} and other non-JSON forms: nothing to check.
    []
  end

  # `['--force']` when the task declares an output that no longer exists.
  def force(root, task)
    outputs(root, task).all?(&:exist?) ? [] : ['--force']
  end
end
